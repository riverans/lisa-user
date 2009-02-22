#include <sys/ioctl.h>

#include "if_generic.h"

int if_parse_generic(const char *name, const char *type)
{
	int ret, n;
	char *fmt;
	size_t len;

	fmt = alloca((len = strlen(type)) + 5);
	strcpy(fmt, type);
	strcpy(fmt + len, "%d%n");

	if (!sscanf(name, fmt, &ret, &n))
		return -1;

	if (strlen(name) != n)
		return -1;

	return ret;
}

int if_get_index(const char *name, int sock_fd)
{
	struct ifreq ifr;
	int ret = 0;

	if (strlen(name) >= IFNAMSIZ)
		return 0;

	strcpy(ifr.ifr_name, name);

	assert(sock_fd != -1);

	if (!ioctl(sock_fd, SIOCGIFINDEX, &ifr))
		ret = ifr.ifr_ifindex;

	return ret;
}

char *if_get_name(int if_index, int sock_fd, char *name)
{
	struct ifreq ifr;

	assert(sock_fd != -1);

	ifr.ifr_ifindex = if_index;

	if (ioctl(sock_fd, SIOCGIFNAME, &ifr))
		return NULL;

	if (!name)
		return strdup(ifr.ifr_name);

	strncpy(name, ifr.ifr_name, IFNAMSIZ);	

	return name;
}

struct build_ip_list_priv {
	struct list_head *lh;
	int ifindex;
};

static int if_build_ip_list(const struct sockaddr_nl *who, const struct nlmsghdr *n, 
		void *__arg)
{
	struct ifaddrmsg *ifa = NLMSG_DATA(n);
	struct rtattr *rta_tb[IFA_MAX+1];
	struct build_ip_list_priv *arg = __arg;

	if (n->nlmsg_type != RTM_NEWADDR)
		return 0;
	if (n->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa)))
		return 0;

	if (arg->ifindex && arg->ifindex != ifa->ifa_index)
		return 0;

	parse_rtattr(rta_tb, IFA_MAX, IFA_RTA(ifa), n->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa)));

	// FIXME for now all interfaces seem to have the same address in both
	// fields (IFA_LOCAL and IFA_ADDRESS)
	if (rta_tb[IFA_LOCAL]) {
		struct if_addr *addr;

		addr = malloc(sizeof(*addr));
		if (addr == NULL)
			return 0;

		addr->ifindex = ifa->ifa_index;
		addr->af = ifa->ifa_family;
		addr->inet = *(struct in_addr *)RTA_DATA(rta_tb[IFA_LOCAL]);
		addr->prefixlen = ifa->ifa_prefixlen;

		list_add_tail(&addr->lh, arg->lh);
	}

	return 0;
}

int if_get_addr(int ifindex, int af, struct list_head *lh, struct rtnl_handle *rth)
{
	struct rtnl_handle __rth;
	struct build_ip_list_priv priv = {
		.ifindex = ifindex,
		.lh = lh
	};

	if (rth == NULL && rtnl_open(rth = &__rth))
		return -1;

	// FIXME we must check if passing af to rtnl_wilddump_request
	// is safe and meaningful
	if (rtnl_wilddump_request(rth, af, RTM_GETADDR) < 0) {
		perror("Cannot send dump request"); // FIXME output
		return -1;
	}

	if (rtnl_dump_filter(rth, if_build_ip_list, &priv) < 0) {
		fprintf(stderr, "Dump terminated\n"); // FIXME output
		return -1;
	} 

	if (rth == &__rth)
		rtnl_close(rth);

	return 0;
}

int if_change_addr(int cmd, int ifindex, struct in_addr addr, int prefixlen, int secondary, struct rtnl_handle *rth)
{
	struct rtnl_handle __rth;
	struct {
		struct nlmsghdr 	n;
		struct ifaddrmsg 	ifa;
		char   				buf[256];
	} req;
	int ret;

	if (rth == NULL && rtnl_open(rth = &__rth))
		return -1;
	
	/* Initialize request argument */
	memset(&req, 0, sizeof(req));
	
	/* setup struct nlmsghdr */
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = cmd;

	/* setup struct ifaddrmsg */
	req.ifa.ifa_index = ifindex;
	req.ifa.ifa_family = AF_INET;
	if (secondary)
		req.ifa.ifa_flags |= IFA_F_SECONDARY;
	req.ifa.ifa_prefixlen = prefixlen;
	req.ifa.ifa_scope = *(uint8_t *)&addr.s_addr == 127 ? RT_SCOPE_HOST : 0;
	
	/* Complete request with the parsed inet_prefix */
	addattr_l(&req.n, sizeof(req), IFA_LOCAL, &addr.s_addr, prefixlen);
	addattr_l(&req.n, sizeof(req), IFA_ADDRESS, &addr.s_addr, prefixlen);
	
	/* Make request */
	if ((ret = rtnl_talk(rth, &req.n)) < 0)
		return ret;
	
	/* Close netlink socket */	
	if (rth == &__rth)
		rtnl_close(rth);

	return 0;
}
