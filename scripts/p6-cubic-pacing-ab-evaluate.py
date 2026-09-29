#!/usr/bin/env python3
import sys
from pathlib import Path

def parse(path):
    text=Path(path).read_text().strip()
    fields={}
    for token in text.split():
        if "=" in token:
            k,v=token.split("=",1); fields[k]=v
    return text,fields

if len(sys.argv)!=4:
    raise SystemExit("usage: p6-cubic-pacing-ab-evaluate.py paced-summary bypass-summary linux-summary")

paced_text,paced=parse(sys.argv[1])
bypass_text,bypass=parse(sys.argv[2])
linux_text,linux=parse(sys.argv[3])

def need(d,k):
    if k not in d: raise SystemExit(f"missing {k}")
    return d[k]

for key in ("base_rtt_ms","rate_mbit","loss_mode","fault_marker_count","fault_marker_gap_packets","fault_marker_prefix","bdp_bytes","queue_pkts"):
    expected=need(paced,key)
    if need(bypass,key)!=expected or need(linux,key)!=expected:
        raise SystemExit(f"path mismatch for {key}")
if need(paced,"payload_bytes") != need(bypass,"payload_bytes"):
    raise SystemExit("tcp-shift A/B payload mismatch")

if need(paced,"cc")!="cubic" or need(bypass,"cc")!="cubic" or need(linux,"cc")!="cubic":
    raise SystemExit("CUBIC A/B requires cubic on all paths")
expected=int(need(paced,"fault_marker_count"))
for label,d,key in (("paced",paced,"retransmit_events"),("bypass",bypass,"retransmit_events"),("linux",linux,"total_retrans")):
    if int(need(d,key))!=expected:
        raise SystemExit(f"{label} retransmission mismatch")
for label,d in (("paced",paced),("bypass",bypass)):
    if int(need(d,"timeout_events"))!=0:
        raise SystemExit(f"{label} fell back to RTO")
    if need(d,"qdisc_drops")!="0/0":
        raise SystemExit(f"{label} had unrelated qdisc drops")
if int(need(paced,"pacing_deferrals"))<1 or int(need(paced,"pacing_tx_bytes"))<1:
    raise SystemExit("paced control did not exercise userspace pacing")
if int(need(bypass,"pacing_deferrals"))!=0 or int(need(bypass,"pacing_resumes"))!=0:
    raise SystemExit("gate-bypass variant still deferred through the scheduler")
if int(need(bypass,"pacing_tx_bytes"))<1:
    raise SystemExit("gate-bypass variant lost pacing flow/clock accounting")

pg=float(need(paced,"goodput_mbps"))
ug=float(need(bypass,"goodput_mbps"))
lg=float(need(linux,"goodput_mbps"))
if min(pg,ug,lg)<=0: raise SystemExit("non-positive goodput")

print(
    "p6_cubic_pacing_ab=ok "
    f"paced_goodput_mbps={pg:.6f} gate_bypass_goodput_mbps={ug:.6f} linux_goodput_mbps={lg:.6f} "
    f"gate_bypass_over_paced={ug/pg:.6f} paced_over_linux={pg/lg:.6f} gate_bypass_over_linux={ug/lg:.6f} "
    f"paced_loss_events={need(paced,'loss_events')} gate_bypass_loss_events={need(bypass,'loss_events')} "
    f"paced_max_delivery_rate_Bps={need(paced,'max_rate_bytes_per_sec')} "
    f"gate_bypass_max_delivery_rate_Bps={need(bypass,'max_rate_bytes_per_sec')}"
)
print("paced_summary="+paced_text)
print("gate_bypass_summary="+bypass_text)
print("linux_summary="+linux_text)
