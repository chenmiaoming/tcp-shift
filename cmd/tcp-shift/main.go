//go:build linux

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"gvisor.dev/gvisor/pkg/rawfile"
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/link/fdbased"
	"gvisor.dev/gvisor/pkg/tcpip/link/tun"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
)

const (
	nicID tcpip.NICID = 1

	// TCP_CONGESTION from linux/uapi/linux/tcp.h. The standard syscall package
	// exposes SetsockoptString but does not consistently expose this Linux-only
	// option across Go toolchain versions.
	linuxTCP_CONGESTION = 13
)

var (
	buildVersion   = "dev"
	gvisorRef      = "unknown"
	gvisorRevision = "unknown"
)

var copyBuffers = sync.Pool{
	New: func() any {
		b := make([]byte, 32<<10)
		return &b
	},
}

type config struct {
	engine        string
	tunName       string
	listen        string
	backend       string
	cc            string
	recovery      string
	tcpBufferMiB  int
	statsInterval time.Duration
}

func main() {
	cfg := config{}
	showVersion := flag.Bool("version", false, "print tcp-shift and gVisor build revisions")
	flag.StringVar(&cfg.engine, "engine", "netstack", "frontend engine: netstack or native")
	flag.StringVar(&cfg.tunName, "tun", "ts0", "existing TUN interface owned by the current user")
	flag.StringVar(&cfg.listen, "listen", "10.99.0.2:5201", "WAN-facing TCP listen address")
	flag.StringVar(&cfg.backend, "backend", "127.0.0.1:5202", "host-kernel TCP backend")
	flag.StringVar(&cfg.cc, "cc", "cubic", "congestion control: reno, cubic, or bbr")
	flag.StringVar(&cfg.recovery, "recovery", "rack", "gVisor TCP loss recovery: rack or legacy")
	flag.IntVar(&cfg.tcpBufferMiB, "tcp-buffer-mib", 1, "gVisor TCP send/receive buffer size in MiB")
	flag.DurationVar(&cfg.statsInterval, "stats-interval", 0, "periodically print Go/netstack memory statistics (0 disables)")
	flag.Parse()

	if *showVersion {
		fmt.Printf("tcp-shift %s\ngvisor-ref %s\ngvisor-sha %s\n", buildVersion, gvisorRef, gvisorRevision)
		return
	}

	cfg.engine = strings.ToLower(cfg.engine)
	if cfg.engine != "netstack" && cfg.engine != "native" {
		log.Fatalf("unsupported --engine=%q", cfg.engine)
	}
	if cfg.tcpBufferMiB < 1 || cfg.tcpBufferMiB > 64 {
		log.Fatalf("--tcp-buffer-mib must be in [1, 64]")
	}
	cfg.cc = strings.ToLower(cfg.cc)
	if cfg.cc != "reno" && cfg.cc != "cubic" && cfg.cc != "bbr" {
		log.Fatalf("unsupported --cc=%q", cfg.cc)
	}
	cfg.recovery = strings.ToLower(cfg.recovery)
	if cfg.recovery != "rack" && cfg.recovery != "legacy" {
		log.Fatalf("unsupported --recovery=%q", cfg.recovery)
	}

	var (
		s        *stack.Stack
		listener net.Listener
		err      error
	)
	if cfg.engine == "native" {
		if err := validateNativeCongestionControl(cfg.cc); err != nil {
			log.Fatalf("native congestion control %q unavailable: %v", cfg.cc, err)
		}
		listener, err = net.Listen("tcp", cfg.listen)
	} else {
		s, listener, err = newStack(cfg)
	}
	if err != nil {
		log.Fatal(err)
	}
	defer listener.Close()
	if s != nil {
		defer s.Close()
		defer s.Wait()
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if cfg.statsInterval > 0 {
		go logStats(ctx, s, cfg.statsInterval)
	}

	log.Printf("ready: engine=%s tun=%s listen=%s backend=%s cc=%s recovery=%s tcp_buffer=%dMiB gvisor=%s", cfg.engine, cfg.tunName, cfg.listen, cfg.backend, cfg.cc, cfg.recovery, cfg.tcpBufferMiB, gvisorRevision)

	go func() {
		<-ctx.Done()
		_ = listener.Close()
	}()

	for {
		front, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return
			}
			log.Printf("accept: %v", err)
			continue
		}
		if cfg.engine == "native" {
			// Set the accepted WAN-facing socket explicitly. This makes the native
			// CUBIC/BBR baseline independent of the host's global sysctl default.
			if err := setNativeCongestionControl(front, cfg.cc); err != nil {
				log.Printf("set native congestion control %q: %v", cfg.cc, err)
				_ = front.Close()
				continue
			}
		}
		go relay(front, cfg.backend)
	}
}

func validateNativeCongestionControl(cc string) error {
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, syscall.IPPROTO_TCP)
	if err != nil {
		return err
	}
	defer syscall.Close(fd)
	return syscall.SetsockoptString(fd, syscall.IPPROTO_TCP, linuxTCP_CONGESTION, cc)
}

func setNativeCongestionControl(conn net.Conn, cc string) error {
	sc, ok := conn.(syscall.Conn)
	if !ok {
		return fmt.Errorf("connection type %T has no syscall access", conn)
	}
	raw, err := sc.SyscallConn()
	if err != nil {
		return err
	}
	var sockErr error
	if err := raw.Control(func(fd uintptr) {
		sockErr = syscall.SetsockoptString(int(fd), syscall.IPPROTO_TCP, linuxTCP_CONGESTION, cc)
	}); err != nil {
		return err
	}
	return sockErr
}

func newStack(cfg config) (*stack.Stack, net.Listener, error) {
	host, portText, err := net.SplitHostPort(cfg.listen)
	if err != nil {
		return nil, nil, fmt.Errorf("parse --listen: %w", err)
	}
	port64, err := strconv.ParseUint(portText, 10, 16)
	if err != nil || port64 == 0 {
		return nil, nil, fmt.Errorf("invalid listen port %q", portText)
	}

	ip := net.ParseIP(host)
	if ip == nil {
		return nil, nil, fmt.Errorf("listen host must be an IP address, got %q", host)
	}

	var (
		addr  tcpip.Address
		proto tcpip.NetworkProtocolNumber
	)
	if v4 := ip.To4(); v4 != nil {
		addr = tcpip.AddrFrom4Slice(v4)
		proto = ipv4.ProtocolNumber
	} else {
		addr = tcpip.AddrFrom16Slice(ip.To16())
		proto = ipv6.ProtocolNumber
	}

	s := stack.New(stack.Options{
		NetworkProtocols:   []stack.NetworkProtocolFactory{ipv4.NewProtocol, ipv6.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol},
	})
	cleanupOnError := true
	defer func() {
		if cleanupOnError {
			s.Close()
			s.Wait()
		}
	}()

	mtu, err := rawfile.GetMTU(cfg.tunName)
	if err != nil {
		return nil, nil, fmt.Errorf("get MTU for %s: %w", cfg.tunName, err)
	}
	fd, err := tun.Open(cfg.tunName)
	if err != nil {
		return nil, nil, fmt.Errorf("open TUN %s: %w", cfg.tunName, err)
	}

	linkEP, err := fdbased.New(&fdbased.Options{
		FDs:            []int{fd},
		MTU:            mtu,
		EthernetHeader: false,
	})
	if err != nil {
		return nil, nil, fmt.Errorf("create fdbased endpoint: %w", err)
	}
	if err := s.CreateNIC(nicID, linkEP); err != nil {
		return nil, nil, fmt.Errorf("CreateNIC: %s", err)
	}
	if err := s.AddProtocolAddress(nicID, tcpip.ProtocolAddress{
		Protocol:          proto,
		AddressWithPrefix: addr.WithPrefix(),
	}, stack.AddressProperties{}); err != nil {
		return nil, nil, fmt.Errorf("AddProtocolAddress: %s", err)
	}

	zero := make([]byte, addr.BitLen()/8)
	subnet, err := tcpip.NewSubnet(tcpip.AddrFromSlice(zero), tcpip.MaskFromBytes(zero))
	if err != nil {
		return nil, nil, fmt.Errorf("default subnet: %s", err)
	}
	s.SetRouteTable([]tcpip.Route{{Destination: subnet, NIC: nicID}})

	buf := cfg.tcpBufferMiB << 20
	sendBuf := tcpip.TCPSendBufferSizeRangeOption{Min: 4 << 10, Default: buf, Max: buf}
	if err := s.SetTransportProtocolOption(tcp.ProtocolNumber, &sendBuf); err != nil {
		return nil, nil, fmt.Errorf("set TCP send buffer: %s", err)
	}
	recvBuf := tcpip.TCPReceiveBufferSizeRangeOption{Min: 4 << 10, Default: buf, Max: buf}
	if err := s.SetTransportProtocolOption(tcp.ProtocolNumber, &recvBuf); err != nil {
		return nil, nil, fmt.Errorf("set TCP receive buffer: %s", err)
	}
	moderate := tcpip.TCPModerateReceiveBufferOption(false)
	if err := s.SetTransportProtocolOption(tcp.ProtocolNumber, &moderate); err != nil {
		return nil, nil, fmt.Errorf("disable TCP receive autotuning: %s", err)
	}
	sack := tcpip.TCPSACKEnabled(true)
	if err := s.SetTransportProtocolOption(tcp.ProtocolNumber, &sack); err != nil {
		return nil, nil, fmt.Errorf("enable SACK: %s", err)
	}
	recovery := tcpip.TCPRecovery(0)
	if cfg.recovery == "rack" {
		recovery = tcpip.TCPRACKLossDetection
	}
	if err := s.SetTransportProtocolOption(tcp.ProtocolNumber, &recovery); err != nil {
		return nil, nil, fmt.Errorf("set TCP recovery %q: %s", cfg.recovery, err)
	}
	cc := tcpip.CongestionControlOption(cfg.cc)
	if err := s.SetTransportProtocolOption(tcp.ProtocolNumber, &cc); err != nil {
		return nil, nil, fmt.Errorf("set congestion control %q: %s", cfg.cc, err)
	}

	listener, err := gonet.ListenTCP(s, tcpip.FullAddress{NIC: nicID, Addr: addr, Port: uint16(port64)}, proto)
	if err != nil {
		return nil, nil, fmt.Errorf("listen %s in netstack: %w", cfg.listen, err)
	}

	cleanupOnError = false
	return s, listener, nil
}

func relay(front net.Conn, backend string) {
	defer front.Close()
	d := net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}
	back, err := d.Dial("tcp", backend)
	if err != nil {
		log.Printf("backend %s: %v", backend, err)
		return
	}
	defer back.Close()

	var wg sync.WaitGroup
	wg.Add(2)
	copyOneWay := func(dst, src net.Conn) {
		defer wg.Done()
		bp := copyBuffers.Get().(*[]byte)
		_, _ = io.CopyBuffer(dst, src, *bp)
		copyBuffers.Put(bp)
		if cw, ok := dst.(interface{ CloseWrite() error }); ok {
			_ = cw.CloseWrite()
		}
	}
	go copyOneWay(back, front)
	go copyOneWay(front, back)
	wg.Wait()
}

func logStats(ctx context.Context, s *stack.Stack, every time.Duration) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			var m runtime.MemStats
			runtime.ReadMemStats(&m)
			if s == nil {
				log.Printf("mem heap_alloc=%dMiB heap_sys=%dMiB sys=%dMiB goroutines=%d",
					m.HeapAlloc>>20, m.HeapSys>>20, m.Sys>>20, runtime.NumGoroutine())
			} else {
				log.Printf("mem heap_alloc=%dMiB heap_sys=%dMiB sys=%dMiB goroutines=%d tcp=%+v",
					m.HeapAlloc>>20, m.HeapSys>>20, m.Sys>>20, runtime.NumGoroutine(), s.Stats().TCP)
			}
		}
	}
}
