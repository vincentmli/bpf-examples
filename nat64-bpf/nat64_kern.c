/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2021 Toke Høiland-Jørgensen <toke@toke.dk> */


#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <linux/pkt_sched.h>
#include <linux/pkt_cls.h>
#include <stdbool.h>
#include "../include/xdp/parsing_helpers.h"
#include "nat64.h"

char _license[] SEC("license") = "GPL";

struct nat64_config config;

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, struct in6_addr);
	__type(value, struct v6_addr_state);
	__uint(max_entries, 1);
	__uint(map_flags, BPF_F_NO_PREALLOC);
} v6_state_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u32);
	__type(value, struct in6_addr);
	__uint(max_entries, 1);
	__uint(map_flags, BPF_F_NO_PREALLOC);
} v4_reversemap SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LPM_TRIE);
	__uint(key_size, sizeof(struct in6_addr));
	__uint(value_size, sizeof(__u32));
	__uint(max_entries, 1);
	__uint(map_flags, BPF_F_NO_PREALLOC);
} allowed_v6_src SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_QUEUE);
	__uint(key_size, 0);
	__uint(value_size, sizeof(__u32));
	__uint(max_entries, 1);
} reclaimed_addrs SEC(".maps");

static int nat64_handle_v4(struct __sk_buff *skb, struct hdr_cursor *nh)
{
	void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;

        int ip_type, iphdr_len, ip_offset;
        struct in6_addr *dst_v6;
	struct ipv6hdr *ip6h;
        int ret = TC_ACT_OK;
	struct iphdr *iph;
        struct ethhdr *eth;
        __u32 dst_v4;

	struct ipv6hdr dst_hdr = {
		.version = 6,
		.saddr = config.v6_prefix,
	};

        ip_offset = (nh->pos - data) & 0x1fff;

        ip_type = parse_iphdr(nh, data_end, &iph);
        if (ip_type < 0)
                goto out;

        dst_v4 = bpf_ntohl(iph->daddr);
        if ((dst_v4 & config.v4_mask) != config.v4_prefix)
                goto out;

        /* At this point we know the destination IP is within the configured
         * subnet, so if we can't rewrite the packet it should be dropped (so as
         * not to leak traffic in that subnet).
         */
        ret = TC_ACT_SHOT;

        iphdr_len = iph->ihl * 4;
        /* drop packets with IP options */
        if (iphdr_len != sizeof(struct iphdr))
                goto out;

        dst_v6 = bpf_map_lookup_elem(&v4_reversemap, &dst_v4);
        if (!dst_v6)
                goto out;

        // src v4 as last octet of nat64 address
        dst_hdr.saddr.s6_addr32[3] = iph->saddr;
        dst_hdr.daddr = *dst_v6;
        dst_hdr.nexthdr = iph->protocol;
        dst_hdr.hop_limit = iph->ttl;
        dst_hdr.payload_len = bpf_htons(bpf_ntohs(iph->tot_len) - iphdr_len);

        if (bpf_skb_change_proto(skb, bpf_htons(ETH_P_IPV6), 0))
                goto out;

	data = (void *)(unsigned long long)skb->data;
	data_end = (void *)(unsigned long long)skb->data_end;

        eth = data;
        ip6h = data + ip_offset;
        if (eth + 1 > data_end || ip6h + 1 > data_end)
                goto out;

        eth->h_proto = bpf_htons(ETH_P_IPV6);
        *ip6h = dst_hdr;

        ret = bpf_redirect_neigh(skb->ifindex, NULL, 0, 0);
out:
        return ret;
}

static long check_item(struct bpf_map *map, const void *key, void *value, void *ctx)
{
        struct v6_addr_state *state = value;
        __u64 timeout = *((__u64 *)ctx);

        if (state->last_seen < timeout && !state->static_conf) {
                __u32 v4_addr = state->v4_addr;
                bpf_map_delete_elem(map, key);
                bpf_map_delete_elem(&v4_reversemap, &v4_addr);
                bpf_map_push_elem(&reclaimed_addrs, &v4_addr, 0);

                /* only reclaim one address at a time, so mappings don't expire
                 * until they absolutely have to
                 */
                return 1;
        }

        return 0;
}

static __u32 reclaim_v4_addr(void)
{
        __u64 timeout = bpf_ktime_get_ns() - config.timeout_ns;
        __u32 src_v4;

        if (bpf_map_pop_elem(&reclaimed_addrs, &src_v4) == 0)
                return src_v4;

        bpf_for_each_map_elem(&v6_state_map, check_item, &timeout, 0);

        return bpf_map_pop_elem(&reclaimed_addrs, &src_v4) ? 0 : src_v4;
}

static struct v6_addr_state *alloc_new_state(struct in6_addr *src_v6)
{
        struct v6_addr_state new_v6_state = { .last_seen = bpf_ktime_get_ns() };
        __u32 max_v4 = (config.v4_prefix | ~config.v4_mask) - 1;
        __u32 src_v4 = 0;
        int i;

        for (i = 0; i < 10; i++) {
                __u32 next_v4, next_addr;

                next_addr = __sync_fetch_and_add(&config.next_addr, 0);
                next_v4 = config.v4_prefix + next_addr;

                if (next_v4 >= max_v4) {
                        src_v4 = reclaim_v4_addr();
                        break;
                }

                if (__sync_val_compare_and_swap(&config.next_addr,
                                                next_addr,
                                                next_addr + 1) == next_addr) {
                        src_v4 = next_v4;
                        break;
                        }
        }

        /* If src_v4 is 0 here, we failed to find an available addr */
        if (!src_v4)
                return NULL;

        new_v6_state.v4_addr = src_v4;
        if (bpf_map_update_elem(&v6_state_map, src_v6, &new_v6_state, BPF_NOEXIST))
                goto err;
        if (bpf_map_update_elem(&v4_reversemap, &src_v4, src_v6, BPF_NOEXIST))
                goto err_v4;

        return bpf_map_lookup_elem(&v6_state_map, src_v6);

err_v4:
        bpf_map_delete_elem(&v6_state_map, src_v6);
err:
        /* failed to insert entry in maps, put the address back in the queue for
         * reclaiming
         */
        bpf_map_push_elem(&reclaimed_addrs, &src_v4, 0);
        return NULL;
}

static int cmp_v6addr(struct in6_addr *a, struct in6_addr *b)
{
        int i;
        for (i = 0; i < 4; i++) {
                if (a->s6_addr32[i] < b->s6_addr32[i])
                        return -1;
                if (a->s6_addr32[i] > b->s6_addr32[i])
                        return 1;
        }
        return 0;
}

static __always_inline __u16 csum_fold_helper(__u32 csum)
{
	__u32 sum;
	sum = (csum >> 16) + (csum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}

static int nat64_handle_v6(struct __sk_buff *skb, struct hdr_cursor *nh)
{
	void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;

	struct v6_trie_key saddr_key = { .t.prefixlen = 128 };
        struct in6_addr *dst_v6, src_v6, subnet_v6 = {};
        int ip_type, ip_offset;
	struct ipv6hdr *ip6h;
        int ret = TC_ACT_OK;
	struct ethhdr *eth;
        struct iphdr *iph;
        __u32 *allowval;

        struct v6_addr_state *v6_state;

	struct iphdr dst_hdr = {
		.version = 4,
                .ihl = 5,
        };

        ip_offset = (nh->pos - data) & 0x1fff;

        ip_type = parse_ip6hdr(nh, data_end, &ip6h);
        if (ip_type < 0)
                goto out;

        dst_v6 = &ip6h->daddr;
        subnet_v6 = *dst_v6;
        /* v6 pxlen is always 96 */
        subnet_v6.s6_addr32[3] = 0;
        if (cmp_v6addr(&subnet_v6, &config.v6_prefix))
                goto out;

        /* At this point we know the destination IP is within the configured
         * subnet, so if we can't rewrite the packet it should be dropped (so as
         * not to leak traffic in that subnet).
         */
        ret = TC_ACT_SHOT;

        /* drop packets with IP options - parser skips options */
        if (ip_type != ip6h->nexthdr)
                goto out;

        saddr_key.addr = ip6h->saddr;
        allowval = bpf_map_lookup_elem(&allowed_v6_src, &saddr_key);
        if (!allowval)
                goto out;

        src_v6 = ip6h->saddr;
        v6_state = bpf_map_lookup_elem(&v6_state_map, &src_v6);
        if (!v6_state) {
                v6_state = alloc_new_state(&src_v6);
                if (!v6_state)
                        goto out;
        } else {
                v6_state->last_seen = bpf_ktime_get_ns();
                bpf_map_update_elem(&v6_state_map, &src_v6, v6_state, BPF_EXIST);
        }

        dst_hdr.daddr = ip6h->daddr.s6_addr32[3];
        dst_hdr.saddr = bpf_htonl(v6_state->v4_addr);
        dst_hdr.protocol = ip6h->nexthdr;
        dst_hdr.ttl = ip6h->hop_limit;
        dst_hdr.tot_len = bpf_htons(bpf_ntohs(ip6h->payload_len) + sizeof(dst_hdr));
        dst_hdr.check = csum_fold_helper(bpf_csum_diff((__be32 *)&dst_hdr, 0,
                                                       (__be32 *)&dst_hdr, sizeof(dst_hdr),
                                                       0));

        if (bpf_skb_change_proto(skb, bpf_htons(ETH_P_IP), 0))
                goto out;

	data = (void *)(unsigned long long)skb->data;
	data_end = (void *)(unsigned long long)skb->data_end;

        eth = data;
        iph = data + ip_offset;
        if (eth + 1 > data_end || iph + 1 > data_end)
                goto out;

        eth->h_proto = bpf_htons(ETH_P_IP);
        *iph = dst_hdr;

        ret = bpf_redirect(skb->ifindex, BPF_F_INGRESS);
out:
        return ret;
}

static int nat64_handler(struct __sk_buff *skb, bool egress)
{
	void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;
	struct hdr_cursor nh  = { .pos = data };
	struct ethhdr *eth;
	int eth_type;

	/* Parse Ethernet and IP/IPv6 headers */
	eth_type = parse_ethhdr(&nh, data_end, &eth);
	if (eth_type == bpf_htons(ETH_P_IP) && egress)
		return nat64_handle_v4(skb, &nh);
	else if (eth_type == bpf_htons(ETH_P_IPV6) && !egress)
		return nat64_handle_v6(skb, &nh);

	return TC_ACT_OK;
}
SEC("classifier")
int nat64_egress(struct __sk_buff *skb)
{
        return nat64_handler(skb, true);
}

SEC("classifier")
int nat64_ingress(struct __sk_buff *skb)
{
        return nat64_handler(skb, false);
}
