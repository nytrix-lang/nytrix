/*
 * Fixture: parse the host TCP header's endian-selected anonymous fields.
 */
#include <netinet/tcp.h>

int main(void) {
  struct tcphdr packet = {0};
  packet.th_sport = 0x1234;
  return packet.source != 0x1234;
}
