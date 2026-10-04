#ifndef TCP_SHIFT_HOST_NFT_CONTROL_H
#define TCP_SHIFT_HOST_NFT_CONTROL_H

#include <stdio.h>

int tcp_shift_nft_control_probe(void);
int tcp_shift_nft_control_run(const char *commands,
                              int dry_run,
                              FILE *output_stream,
                              FILE *error_stream);

#endif /* TCP_SHIFT_HOST_NFT_CONTROL_H */
