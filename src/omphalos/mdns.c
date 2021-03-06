#include <ctype.h>
#include <assert.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <sys/socket.h>
#include <omphalos/tx.h>
#include <netinet/ip6.h>
#include <omphalos/ip.h>
#include <omphalos/udp.h>
#include <omphalos/dns.h>
#include <omphalos/mdns.h>
#include <omphalos/csum.h>
#include <asm/byteorder.h>
#include <omphalos/diag.h>
#include <omphalos/route.h>
#include <omphalos/service.h>
#include <omphalos/hwaddrs.h>
#include <omphalos/ethernet.h>
#include <omphalos/netaddrs.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

#define MDNS_NET4 __constant_htonl(0xe00000fbul)

struct natpmphdr {
	uint8_t ver;
	uint8_t op;
};

void handle_natpmp_packet(omphalos_packet *op,const void *frame,size_t len){
	const struct natpmphdr *nat = frame;
	size_t plen;

	if(len < sizeof(*nat)){
		diagnostic("[%s] malformed NAT-PMP (%zub)",op->i->name,len);
		goto malformed;
	}
	plen = len - sizeof(*nat);
	if(nat->ver){
		diagnostic("[%s] bad NAT-PMP version (%u)",op->i->name,nat->ver);
		op->noproto = 1;
		return;
	}
	// We probably ought enforce the address/port/state
	switch(nat->op){
	case 0: // public address request
		if(plen){
			diagnostic("[%s] bad NAT-PMP PAR len (%zub)",op->i->name,plen);
			goto malformed;
		}
		break;
	case 1: case 2: // port mapping request
		if(plen != 10){
			diagnostic("[%s] bad NAT-PMP PMR len (%zub)",op->i->name,plen);
			goto malformed;
		}
		break;
	case 128: // public address response
		if(plen != 10){
			diagnostic("[%s] bad NAT-PMP PA len (%zub)",op->i->name,plen);
			goto malformed;
		}
		break;
	case 129: case 130: // port mapping response
		if(plen != 14){
			diagnostic("[%s] bad NAT-PMP PM len (%zub)",op->i->name,plen);
			goto malformed;
		}
		break;
	default:
		diagnostic("[%s] bad NAT-PMP op (%u)",op->i->name,nat->op);
		break;
	}
	return;

malformed:
	op->malformed = 1;
}

void handle_mdns_packet(omphalos_packet *op,const void *frame,size_t len){
	if(handle_dns_packet(op,frame,len) == 1){
		observe_service(op->i,op->l2s,op->l3s,op->l3proto,
				op->l4src,L"mDNS",NULL);
	}
}

int tx_mdns_ptr(interface *i,const char *str,int fam,const void *lookup){
	const void *mcast_addr;
	struct routepath rp;
	int ret = -1;
	size_t flen;
	void *frame;

	if(!(i->flags & IFF_MULTICAST)){
		return 0;
	}
	rp.i = i;
	if(get_source_address(i,AF_INET,fam == AF_INET ? lookup : NULL,&rp.src)){
		uint32_t mcast_netaddr;

		mcast_addr = "\x01\x00\x5e\x00\x00\xfb";
		if((rp.l2 = lookup_l2host(i,mcast_addr)) == NULL){
			return -1;
		}
		mcast_netaddr = MDNS_NET4;
		if( (rp.l3 = lookup_local_l3host(NULL,i,rp.l2,AF_INET,&mcast_netaddr)) ){
			if( (frame = get_tx_frame(i,&flen)) ){
				if(setup_dns_ptr(&rp,AF_INET,&mcast_netaddr,
							MDNS_UDP_PORT,flen,frame,str,
							htons(MDNS_UDP_PORT))){
					abort_tx_frame(i,frame);
				}else{
					send_tx_frame(i,frame);
					ret = 0;
				}
			}
		}
	}
	if(get_source_address(i,AF_INET6,fam == AF_INET6 ? lookup : NULL,&rp.src)){
		uint128_t mcast_netaddr;

		mcast_addr = "\x33\x33\x00\x00\x00\xfb";
		if((rp.l2 = lookup_l2host(i,mcast_addr)) == NULL){
			return -1;
		}
		mcast_netaddr[0] = __constant_htonl(0xff020000u);
		mcast_netaddr[1] = __constant_htonl(0x00000000u);
		mcast_netaddr[2] = __constant_htonl(0x00000000u);
		mcast_netaddr[3] = __constant_htonl(0x000000fbu);
		if((rp.l3 = lookup_local_l3host(NULL,i,rp.l2,AF_INET6,&mcast_netaddr)) == NULL){
			return -1;
		}
		if((frame = get_tx_frame(i,&flen)) == NULL){
			return -1;
		}
		if(setup_dns_ptr(&rp,AF_INET6,mcast_netaddr,MDNS_UDP_PORT,flen,frame,str,htons(MDNS_UDP_PORT))){
			abort_tx_frame(i,frame);
			return -1;
		}
		send_tx_frame(i,frame);
	}
	return ret;
}

#define DOMAIN "local"
#define ENUMSTR "\x09_services\x07""_dns-sd\x04""_udp\x05"DOMAIN
static int
setup_service_enum(char *frame,size_t len){
	struct dnshdr *dns = (struct dnshdr *)frame;
	char *dat;

	if(len < sizeof(*dns) + strlen(ENUMSTR) + 5){
		return -1;
	}
	memset(dns,0,sizeof(*dns)); // mDNS transaction id == 0
	dns->qdcount = htons(1);
	dat = frame + sizeof(*dns);
	strcpy(dat,ENUMSTR);
	*(uint16_t *)(dat + strlen(ENUMSTR) + 1) = DNS_TYPE_PTR;
	*(uint16_t *)(dat + strlen(ENUMSTR) + 3) = DNS_CLASS_IN;
	return sizeof(*dns) + strlen(ENUMSTR) + 5;
}
#undef ENUMSTR
#undef DOMAIN

static int
setup_service_probe(char *frame,size_t len,const char *name){
	struct dnshdr *dns = (struct dnshdr *)frame;
	const char *comp,*c;
	unsigned count;
	char *dat,*d;

	if(len < sizeof(*dns) + strlen(name) + 6){
		return -1;
	}
	memset(dns,0,sizeof(*dns)); // mDNS transaction id == 0
	dat = frame + sizeof(*dns);
	comp = name;
	d = dat;
	if(isprint(*comp)){
		while( (c = strchr(comp,'.')) ){
			*d = c - comp;
			strncpy(d + 1,comp,c - comp);
			d += (c - comp) + 1;
			comp = c + 1;
		}
		*d = strlen(comp);
		strcpy(d + 1,comp);
		d += strlen(comp) + 2;
		*(uint16_t *)(d) = DNS_TYPE_PTR;
		*(uint16_t *)(d + 2) = DNS_CLASS_IN;
		dns->qdcount = htons(1);
		return sizeof(*dns) + (d - dat) + 4;
	}
	// already prepared for dns
	count = 0;
	do{
		unsigned l;
		do{
			l = *(const unsigned char *)comp;
			strncpy(d,comp,l + 1);
			d += l + 1;
			comp += l + 1;
		}while(l != 5 || strncmp(comp - 5,"local",5));
		*d++ = '\0';
		*(uint16_t *)(d) = DNS_TYPE_PTR;
		*(uint16_t *)(d + 2) = DNS_CLASS_IN;
		d += 4;
		++count;
	}while(*comp);
	dns->qdcount = htons(count);
	return sizeof(*dns) + (d - dat);
}

static int
tx_sd4(interface *i,const char *name,const uint32_t *saddr){
	const unsigned char hw[ETH_ALEN] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };
	uint32_t net = MDNS_NET4;
	struct tpacket_hdr *thdr;
	struct udphdr *udp;
	struct iphdr *ip;
	size_t flen,tlen;
	void *frame;
	int r;

	if((frame = get_tx_frame(i,&flen)) == NULL){
		return -1;
	}
	thdr = frame;
	tlen = thdr->tp_mac;
	if((r = prep_eth_header((char *)frame + tlen,flen - tlen,i,
					hw,ETH_P_IP)) < 0){
		abort_tx_frame(i,frame);
		return -1;
	}
	tlen += r;
	ip = (struct iphdr *)((char *)frame + tlen);
	if((r = prep_ipv4_header(ip,flen - tlen,*saddr,net,IPPROTO_UDP)) < 0){
		abort_tx_frame(i,frame);
		return -1;
	}
	tlen += r;
	if(flen - tlen < sizeof(*udp)){
		abort_tx_frame(i,frame);
		return -1;
	}
	udp = (struct udphdr *)((char *)frame + tlen);
	udp->source = htons(MDNS_UDP_PORT);
	udp->dest = htons(MDNS_UDP_PORT);
	tlen += sizeof(*udp);
	if(name == NULL){
		r = setup_service_enum((char *)frame + tlen,flen - tlen);
	}else{
		r = setup_service_probe((char *)frame + tlen,flen - tlen,name);
	}
	if(r < 0){
		abort_tx_frame(i,frame);
		return -1;
	}
	tlen += r;
	ip->tot_len = htons(tlen - ((const char *)ip - (const char *)frame));
	ip->check = ipv4_csum(ip);
	udp->len = htons(ntohs(ip->tot_len) - ip->ihl * 4u);
	udp->check = udp4_csum(ip);
	thdr->tp_len = tlen - thdr->tp_mac;
	return send_tx_frame(i,frame);
}

static int
tx_sd6(interface *i,const char *name,const uint128_t saddr){
	const unsigned char hw[ETH_ALEN] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0xfb };
	uint128_t net = { htonl(0xff020000ul), htonl(0x0ul),
				htonl(0x0ul), htonl(0xfbul) };
	struct tpacket_hdr *thdr;
	struct udphdr *udp;
	struct ip6_hdr *ip;
	size_t flen,tlen;
	void *frame;
	int r;

	if((frame = get_tx_frame(i,&flen)) == NULL){
		return -1;
	}
	thdr = frame;
	tlen = thdr->tp_mac;
	if((r = prep_eth_header((char *)frame + tlen,flen - tlen,i,
					hw,ETH_P_IPV6)) < 0){
		abort_tx_frame(i,frame);
		return -1;
	}
	tlen += r;
	ip = (struct ip6_hdr *)((char *)frame + tlen);
	if((r = prep_ipv6_header(ip,flen - tlen,saddr,net,IPPROTO_UDP)) < 0){
		abort_tx_frame(i,frame);
		return -1;
	}
	tlen += r;
	if(flen - tlen < sizeof(*udp)){
		abort_tx_frame(i,frame);
		return -1;
	}
	udp = (struct udphdr *)((char *)frame + tlen);
	udp->source = htons(MDNS_UDP_PORT);
	udp->dest = htons(MDNS_UDP_PORT);
	tlen += sizeof(*udp);
	if(name == NULL){
		r = setup_service_enum((char *)frame + tlen,flen - tlen);
	}else{
		r = setup_service_probe((char *)frame + tlen,flen - tlen,name);
	}
	if(r < 0){
		abort_tx_frame(i,frame);
		return -1;
	}
	tlen += r;
	thdr->tp_len = tlen;
	ip->ip6_ctlun.ip6_un1.ip6_un1_plen = htons(thdr->tp_len -
		((const char *)udp - (const char *)frame));
	udp->len = ip->ip6_ctlun.ip6_un1.ip6_un1_plen;
	udp->check = udp6_csum(ip);
	thdr->tp_len -= thdr->tp_mac;
	return send_tx_frame(i,frame);
}

int mdns_sd_enumerate(int fam,interface *i,const void *saddr){
	if(!(i->flags & IFF_MULTICAST)){
		return 0;
	}
	if(fam == AF_INET){
		return tx_sd4(i,NULL,saddr);
	}else if(fam == AF_INET6){
		return tx_sd6(i,NULL,saddr);
	}
	return -1;
}

int mdns_sd_probe(int fam,interface *i,const char *name,const void *saddr){
	if(!(i->flags & IFF_MULTICAST)){
		return 0;
	}
	if(fam == AF_INET){
		return tx_sd4(i,name,saddr);
	}else if(fam == AF_INET6){
		return tx_sd6(i,name,saddr);
	}
	return -1;
}

// FIXME these can be combined into a few large requests rather than many
//       lookups with only one query each.
#define TCP(x) x"\x04_tcp\x05local"
#define UDP(x) x"\x04_udp\x05local"
static const char stdsds[] = {
	TCP("\x04_ipp") \
	TCP("\x04_smb")
	TCP("\x04_rfb")
	TCP("\x05_daap")
	TCP("\x05_http")
	TCP("\x05_raop")
	TCP("\x06_adisk")
	TCP("\x08_airplay")
	TCP("\x08_airport")
	TCP("\x08_scanner")
	TCP("\x0b_afpovertcp")
	TCP("\x0c_riousbprint")
	TCP("\x0d_home-sharing")
	TCP("\x0f_pdl-datastream")
	TCP("\x0f_appletv-itunes")
	UDP("\x0c_sleep-proxy")
};
#undef UDP
#undef TCP

// This ought be more of an async launch thing -- it adds too much latency and
// is too bursty at the moment FIXME
int mdns_stdsd_probe(int fam,interface *i,const void *saddr){
	return mdns_sd_probe(fam,i,stdsds,saddr);
}
