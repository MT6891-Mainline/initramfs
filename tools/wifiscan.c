// SPDX-License-Identifier: GPL-2.0-only
/*
 * wifiscan - trigger a cfg80211 scan and print the results.
 *
 * The chopin initramfs has no iw/wpa_supplicant, so this is the smallest
 * generic-netlink client that answers "does the RF side work": bring the
 * interface up, NL80211_CMD_TRIGGER_SCAN with a wildcard SSID (active scan),
 * then poll NL80211_CMD_GET_SCAN and print SSID/BSSID/frequency/signal.
 */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

static int nla_put(struct nlmsghdr *nlh, int maxlen, int type,
		   const void *data, int len)
{
	struct nlattr *a;
	int alen = NLA_HDRLEN + len;
	int newlen = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(alen);

	if (newlen > maxlen)
		return -1;
	a = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
	a->nla_type = type;
	a->nla_len = alen;
	if (len)
		memcpy((char *)a + NLA_HDRLEN, data, len);
	memset((char *)a + alen, 0, NLA_ALIGN(alen) - alen);
	nlh->nlmsg_len = newlen;
	return 0;
}

/* Tolerant chain walker: skip the zero pad word the kernel emits after
 * 64-bit attributes instead of stopping at it.
 */
static struct nlattr *attr_scan(void *base, int len, int type)
{
	int off = 0;

	while (off + 4 <= len) {
		uint32_t v = *(uint32_t *)((char *)base + off);
		int alen = v & 0xffff;
		int atype = (v >> 16) & 0x3fff;

		if (alen < 4) {
			off += 4;
			continue;
		}
		if (atype == type)
			return (struct nlattr *)((char *)base + off);
		if (alen > len - off)
			break;
		off += (alen + 3) & ~3;
	}
	return NULL;
}

static struct nlattr *nla_nest_start(struct nlmsghdr *nlh, int maxlen,
				      int type)
{
	struct nlattr *a;

	if (NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN > maxlen)
		return NULL;
	a = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
	a->nla_type = type | NLA_F_NESTED;
	a->nla_len = NLA_HDRLEN;
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN;
	return a;
}

static void nla_nest_end(struct nlmsghdr *nlh, struct nlattr *a)
{
	a->nla_len = (char *)nlh + nlh->nlmsg_len - (char *)a;
}

static int genl_family_id(int fd, const char *name)
{
	char buf[512];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct iovec iov = { buf, 0 };
	struct msghdr msg = { &sa, sizeof(sa), &iov, 1, NULL, 0, 0 };
	int n, family = -1, rem;
	struct nlattr *a;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = 1;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = CTRL_CMD_GETFAMILY;
	gh->version = 1;
	nla_put(nlh, sizeof(buf), CTRL_ATTR_FAMILY_NAME, name,
		strlen(name) + 1);

	if (sendto(fd, nlh, nlh->nlmsg_len, 0, (void *)&sa, sizeof(sa)) < 0)
		return -1;
	iov.iov_len = sizeof(buf);
	n = recvmsg(fd, &msg, 0);
	if (n < 0)
		return -1;
	if (nlh->nlmsg_type == NLMSG_ERROR)
		return -1;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	a = attr_scan((char *)gh + GENL_HDRLEN,
		      nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN),
		      CTRL_ATTR_FAMILY_ID);
	if (!a)
		return -1;
	rem = a->nla_len - NLA_HDRLEN;
	if (rem >= 2)
		family = *(uint16_t *)((char *)a + NLA_HDRLEN);
	return family;
}

static int if_up(const char *name)
{
	struct ifreq ifr;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
		close(s);
		return -1;
	}
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
		close(s);
		return -1;
	}
	close(s);
	return 0;
}

static int nl_scan_dump(int fd, int family, int ifindex)
{
	char buf[16384];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct timeval tv = { .tv_sec = 2 };
	uint32_t u32;
	int found = 0;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 3;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_GET_SCAN;
	gh->version = 1;
	u32 = ifindex;
	nla_put(nlh, sizeof(buf), NL80211_ATTR_IFINDEX, &u32, sizeof(u32));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (sendto(fd, nlh, nlh->nlmsg_len, 0, (void *)&sa, sizeof(sa)) < 0)
		return -1;

	for (;;) {
		int n = recv(fd, buf, sizeof(buf), 0);

		if (n <= 0)
			break;
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, n);
		     nlh = NLMSG_NEXT(nlh, n)) {
			struct nlattr *a, *bss;
			const uint8_t *bssid = NULL, *ie = NULL;
			uint32_t freq = 0;
			int32_t sig = 0;
			char ssid[33] = "";
			int alen, ielen = 0;

			if (nlh->nlmsg_type == NLMSG_DONE) {
				n = 0;
				break;
			}
			if (nlh->nlmsg_type == NLMSG_ERROR)
				continue;
			gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
			if (gh->cmd != NL80211_CMD_NEW_SCAN_RESULTS)
				continue;
			alen = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
			bss = attr_scan((char *)gh + GENL_HDRLEN, alen,
					NL80211_ATTR_BSS);
			if (!bss)
				continue;
			{
				int bssoff = (char *)bss -
					((char *)gh + GENL_HDRLEN);
				char *bdata = (char *)bss + NLA_HDRLEN;
				int blen = alen - bssoff - NLA_HDRLEN;

				if (bss->nla_len >= NLA_HDRLEN &&
				    (int)bss->nla_len - NLA_HDRLEN < blen)
					blen = bss->nla_len - NLA_HDRLEN;
				a = attr_scan(bdata, blen, NL80211_BSS_BSSID);
				if (a)
					bssid = (const uint8_t *)a + NLA_HDRLEN;
				a = attr_scan(bdata, blen,
					      NL80211_BSS_FREQUENCY);
				if (a)
					freq = *(uint32_t *)((char *)a +
							     NLA_HDRLEN);
				a = attr_scan(bdata, blen,
					      NL80211_BSS_SIGNAL_MBM);
				if (a)
					sig = *(int32_t *)((char *)a +
							   NLA_HDRLEN) / 100;
				a = attr_scan(bdata, blen,
					      NL80211_BSS_INFORMATION_ELEMENTS);
				if (a) {
					ie = (const uint8_t *)a + NLA_HDRLEN;
					ielen = a->nla_len - NLA_HDRLEN;
				}
			}
			if (ie) {
				const uint8_t *p = ie;
				int rem = ielen;
				int k;

				while (rem >= 2 && 2 + p[1] <= rem) {
					if (p[0] == 0) {
						int sl = p[1];

						if (sl > 32)
							sl = 32;
						for (k = 0; k < sl; k++)
							ssid[k] = (p[2 + k] >= 32 &&
								   p[2 + k] < 127)
								  ? p[2 + k] : '.';
						ssid[sl] = 0;
						break;
					}
					rem -= 2 + p[1];
					p += 2 + p[1];
				}
			}
			if (!ssid[0])
				snprintf(ssid, sizeof(ssid), "<hidden>");
			printf("  %-24s %02x:%02x:%02x:%02x:%02x:%02x  %5u MHz  %4d dBm\n",
			       ssid,
			       bssid ? bssid[0] : 0, bssid ? bssid[1] : 0,
			       bssid ? bssid[2] : 0, bssid ? bssid[3] : 0,
			       bssid ? bssid[4] : 0, bssid ? bssid[5] : 0,
			       freq, sig);
			found++;
		}
		if (n == 0)
			break;
	}
	return found;
}

int main(int argc, char **argv)
{
	const char *ifname = argc > 1 ? argv[1] : "wlan0";
	char buf[8192];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct timeval tv = { .tv_sec = 2 };
	int fd, family, ifindex, ret, attempt, total = 0;
	uint32_t u32;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		perror("wifiscan: netlink socket");
		return 1;
	}
	if (bind(fd, (void *)&sa, sizeof(sa)) < 0) {
		perror("wifiscan: bind");
		return 1;
	}
	family = genl_family_id(fd, "nl80211");
	if (family < 0) {
		printf("wifiscan: nl80211 not registered (is cfg80211 up?)\n");
		return 1;
	}
	ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		printf("wifiscan: no interface %s\n", ifname);
		return 1;
	}
	printf("wifiscan: %s ifindex=%d\n", ifname, ifindex);
	if (if_up(ifname))
		printf("wifiscan: SIOCSIFFLAGS: %s\n", strerror(errno));
	usleep(800000);

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = 2;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_TRIGGER_SCAN;
	gh->version = 1;
	u32 = ifindex;
	nla_put(nlh, sizeof(buf), NL80211_ATTR_IFINDEX, &u32, sizeof(u32));
	{
		struct nlattr *ssids = nla_nest_start(nlh, sizeof(buf),
						      NL80211_ATTR_SCAN_SSIDS);

		nla_put(nlh, sizeof(buf), 0, NULL, 0);
		if (ssids)
			nla_nest_end(nlh, ssids);
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (sendto(fd, nlh, nlh->nlmsg_len, 0, (void *)&sa, sizeof(sa)) < 0) {
		perror("wifiscan: trigger send");
		return 1;
	}
	ret = recv(fd, buf, sizeof(buf), 0);
	if (ret <= 0) {
		printf("wifiscan: no trigger ack\n");
	} else if (nlh->nlmsg_type == NLMSG_ERROR &&
		   ((struct nlmsgerr *)NLMSG_DATA(nlh))->error) {
		printf("wifiscan: trigger rejected: %s\n",
		       strerror(-((struct nlmsgerr *)NLMSG_DATA(nlh))->error));
	}

	for (attempt = 0; attempt < 8; attempt++) {
		usleep(2000000);
		total = nl_scan_dump(fd, family, ifindex);
		if (total > 0)
			break;
	}
	if (total <= 0)
		printf("wifiscan: no BSS\n");
	close(fd);
	return total > 0 ? 0 : 2;
}
