#include <linux/icmp.h>
#include <omphalos/icmp.h>
#include <omphalos/diag.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

void handle_icmp_packet(omphalos_packet *op,const void *frame,size_t len){
	const struct icmphdr *icmp = frame;

	if(len < sizeof(*icmp)){
		diagnostic(L"%s malformed with %zu",__func__,len);
		op->malformed = 1;
		return;
	}
	// FIXME
}

void handle_icmp6_packet(omphalos_packet *op,const void *frame,size_t len){
	const struct icmphdr *icmp = frame;

	if(len < sizeof(*icmp)){
		diagnostic(L"%s malformed with %zu",__func__,len);
		op->malformed = 1;
		return;
	}
	// FIXME
}
