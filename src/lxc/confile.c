/* SPDX-License-Identifier: LGPL-2.1+ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define __STDC_FORMAT_MACROS
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "af_unix.h"
#include "conf.h"
#include "config.h"
#include "confile.h"
#include "confile_utils.h"
#include "../include/netns_ifaddrs.h"
#include "log.h"
#include "lxcseccomp.h"
#include "memory_utils.h"
#include "network.h"
#include "parse.h"
#include "storage/storage.h"
#include "utils.h"

#if HAVE_SYS_PERSONALITY_H
#include <sys/personality.h>
#endif

#ifndef HAVE_STRLCPY
#include "include/strlcpy.h"
#endif

#ifndef HAVE_STRLCAT
#include "include/strlcat.h"
#endif

lxc_log_define(confile, lxc);

#define lxc_config_define(name)                                             \
	__hot static int set_config_##name(const char *, const char *,      \
					   struct lxc_conf *, void *);      \
	__hot static int get_config_##name(const char *, char *, int,       \
					   struct lxc_conf *, void *);      \
	__hot static int clr_config_##name(const char *, struct lxc_conf *, \
					   void *);

lxc_config_define(autodev);
lxc_config_define(autodev_tmpfs_size);
lxc_config_define(apparmor_allow_incomplete);
lxc_config_define(apparmor_allow_nesting);
lxc_config_define(apparmor_profile);
lxc_config_define(apparmor_raw);
lxc_config_define(cap_drop);
lxc_config_define(cap_keep);
lxc_config_define(cgroup_controller);
lxc_config_define(cgroup2_controller);
lxc_config_define(cgroup_dir);
lxc_config_define(cgroup_relative);
lxc_config_define(console_buffer_size);
lxc_config_define(console_logfile);
lxc_config_define(console_path);
lxc_config_define(console_rotate);
lxc_config_define(console_size);
lxc_config_define(environment);
lxc_config_define(ephemeral);
lxc_config_define(execute_cmd);
lxc_config_define(group);
lxc_config_define(hooks);
lxc_config_define(hooks_version);
lxc_config_define(idmaps);
lxc_config_define(includefiles);
lxc_config_define(init_cmd);
lxc_config_define(init_cwd);
lxc_config_define(init_gid);
lxc_config_define(init_uid);
lxc_config_define(keyring_session);
lxc_config_define(log_file);
lxc_config_define(log_level);
lxc_config_define(log_syslog);
lxc_config_define(monitor);
lxc_config_define(monitor_signal_pdeath);
lxc_config_define(mount);
lxc_config_define(mount_auto);
lxc_config_define(mount_fstab);
lxc_config_define(namespace_clone);
lxc_config_define(namespace_keep);
lxc_config_define(namespace_share);
lxc_config_define(net);
lxc_config_define(net_flags);
lxc_config_define(net_hwaddr);
lxc_config_define(net_ipv4_address);
lxc_config_define(net_ipv4_gateway);
lxc_config_define(net_ipv6_address);
lxc_config_define(net_ipv6_gateway);
lxc_config_define(net_link);
lxc_config_define(net_l2proxy);
lxc_config_define(net_macvlan_mode);
lxc_config_define(net_ipvlan_mode);
lxc_config_define(net_ipvlan_isolation);
lxc_config_define(net_mtu);
lxc_config_define(net_name);
lxc_config_define(net_nic);
lxc_config_define(net_script_down);
lxc_config_define(net_script_up);
lxc_config_define(net_type);
lxc_config_define(net_veth_mode);
lxc_config_define(net_veth_pair);
lxc_config_define(net_veth_ipv4_route);
lxc_config_define(net_veth_ipv6_route);
lxc_config_define(net_vlan_id);
lxc_config_define(no_new_privs);
lxc_config_define(personality);
lxc_config_define(prlimit);
lxc_config_define(pty_max);
lxc_config_define(rootfs_managed);
lxc_config_define(rootfs_mount);
lxc_config_define(rootfs_options);
lxc_config_define(rootfs_path);
lxc_config_define(seccomp_profile);
lxc_config_define(seccomp_allow_nesting);
lxc_config_define(seccomp_notify_cookie);
lxc_config_define(seccomp_notify_proxy);
lxc_config_define(selinux_context);
lxc_config_define(selinux_context_keyring);
lxc_config_define(signal_halt);
lxc_config_define(signal_reboot);
lxc_config_define(signal_stop);
lxc_config_define(start);
lxc_config_define(tty_max);
lxc_config_define(tty_dir);
lxc_config_define(uts_name);
lxc_config_define(sysctl);
lxc_config_define(proc);

/*
 * Important Note:
 * If a new config option is added to this table, be aware that
 * the order in which the options are places into the table matters.
 * That means that more specific options of a namespace have to be
 * placed above more generic ones.
 *
 * For instance: If lxc.ab is placed before lxc.ab.c, the config option
 * lxc.ab.c will always be matched to lxc.ab. That is, the lxc.ab.c option
 * has to be placed above lxc.ab.
 */
static struct lxc_config_t config_jump_table[] = {
	{ "lxc.arch",                      set_config_personality,                 get_config_personality,                 clr_config_personality,               },
	{ "lxc.apparmor.profile",          set_config_apparmor_profile,            get_config_apparmor_profile,            clr_config_apparmor_profile,          },
	{ "lxc.apparmor.allow_incomplete", set_config_apparmor_allow_incomplete,   get_config_apparmor_allow_incomplete,   clr_config_apparmor_allow_incomplete, },
	{ "lxc.apparmor.allow_nesting",    set_config_apparmor_allow_nesting,      get_config_apparmor_allow_nesting,      clr_config_apparmor_allow_nesting,    },
	{ "lxc.apparmor.raw",              set_config_apparmor_raw,                get_config_apparmor_raw,                clr_config_apparmor_raw,              },
	{ "lxc.autodev.tmpfs.size",        set_config_autodev_tmpfs_size,          get_config_autodev_tmpfs_size,          clr_config_autodev_tmpfs_size,        },
	{ "lxc.autodev",                   set_config_autodev,                     get_config_autodev,                     clr_config_autodev,                   },
	{ "lxc.cap.drop",                  set_config_cap_drop,                    get_config_cap_drop,                    clr_config_cap_drop,                  },
	{ "lxc.cap.keep",                  set_config_cap_keep,                    get_config_cap_keep,                    clr_config_cap_keep,                  },
	{ "lxc.cgroup2",                   set_config_cgroup2_controller,          get_config_cgroup2_controller,          clr_config_cgroup2_controller,        },
	{ "lxc.cgroup.dir",                set_config_cgroup_dir,                  get_config_cgroup_dir,                  clr_config_cgroup_dir,                },
	{ "lxc.cgroup.relative",           set_config_cgroup_relative,             get_config_cgroup_relative,             clr_config_cgroup_relative,           },
	{ "lxc.cgroup",                    set_config_cgroup_controller,           get_config_cgroup_controller,           clr_config_cgroup_controller,         },
	{ "lxc.console.buffer.size",       set_config_console_buffer_size,         get_config_console_buffer_size,         clr_config_console_buffer_size,       },
	{ "lxc.console.logfile",           set_config_console_logfile,             get_config_console_logfile,             clr_config_console_logfile,           },
	{ "lxc.console.path",              set_config_console_path,                get_config_console_path,                clr_config_console_path,              },
	{ "lxc.console.rotate",            set_config_console_rotate,              get_config_console_rotate,              clr_config_console_rotate,            },
	{ "lxc.console.size",              set_config_console_size,                get_config_console_size,                clr_config_console_size,              },
	{ "lxc.environment",               set_config_environment,                 get_config_environment,                 clr_config_environment,               },
	{ "lxc.ephemeral",                 set_config_ephemeral,                   get_config_ephemeral,                   clr_config_ephemeral,                 },
	{ "lxc.execute.cmd",               set_config_execute_cmd,                 get_config_execute_cmd,                 clr_config_execute_cmd,               },
	{ "lxc.group",                     set_config_group,                       get_config_group,                       clr_config_group,                     },
	{ "lxc.hook.autodev",              set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.clone",                set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.destroy",              set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.mount",                set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.post-stop",            set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.pre-mount",            set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.pre-start",            set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.start",                set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.start-host",           set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.stop",                 set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.hook.version",              set_config_hooks_version,               get_config_hooks_version,               clr_config_hooks_version,             },
	{ "lxc.hook",                      set_config_hooks,                       get_config_hooks,                       clr_config_hooks,                     },
	{ "lxc.idmap",                     set_config_idmaps,                      get_config_idmaps,                      clr_config_idmaps,                    },
	{ "lxc.include",                   set_config_includefiles,                get_config_includefiles,                clr_config_includefiles,              },
	{ "lxc.init.cmd",                  set_config_init_cmd,                    get_config_init_cmd,                    clr_config_init_cmd,                  },
	{ "lxc.init.gid",                  set_config_init_gid,                    get_config_init_gid,                    clr_config_init_gid,                  },
	{ "lxc.init.uid",                  set_config_init_uid,                    get_config_init_uid,                    clr_config_init_uid,                  },
	{ "lxc.init.cwd",                  set_config_init_cwd,                    get_config_init_cwd,                    clr_config_init_cwd,                  },
	{ "lxc.keyring.session",           set_config_keyring_session,             get_config_keyring_session,             clr_config_keyring_session            },
	{ "lxc.log.file",                  set_config_log_file,                    get_config_log_file,                    clr_config_log_file,                  },
	{ "lxc.log.level",                 set_config_log_level,                   get_config_log_level,                   clr_config_log_level,                 },
	{ "lxc.log.syslog",                set_config_log_syslog,                  get_config_log_syslog,                  clr_config_log_syslog,                },
	{ "lxc.monitor.unshare",           set_config_monitor,                     get_config_monitor,                     clr_config_monitor,                   },
	{ "lxc.monitor.signal.pdeath",     set_config_monitor_signal_pdeath,       get_config_monitor_signal_pdeath,       clr_config_monitor_signal_pdeath,     },
	{ "lxc.mount.auto",                set_config_mount_auto,                  get_config_mount_auto,                  clr_config_mount_auto,                },
	{ "lxc.mount.entry",               set_config_mount,                       get_config_mount,                       clr_config_mount,                     },
	{ "lxc.mount.fstab",               set_config_mount_fstab,                 get_config_mount_fstab,                 clr_config_mount_fstab,               },
	{ "lxc.namespace.clone",           set_config_namespace_clone,             get_config_namespace_clone,             clr_config_namespace_clone,           },
	{ "lxc.namespace.keep",            set_config_namespace_keep,              get_config_namespace_keep,              clr_config_namespace_keep,            },
	{ "lxc.namespace.share",           set_config_namespace_share,             get_config_namespace_share,             clr_config_namespace_share,           },
	{ "lxc.net.flags",                 set_config_net_flags,                   get_config_net_flags,                   clr_config_net_flags,                 },
	{ "lxc.net.hwaddr",                set_config_net_hwaddr,                  get_config_net_hwaddr,                  clr_config_net_hwaddr,                },
	{ "lxc.net.ipv4.address",          set_config_net_ipv4_address,            get_config_net_ipv4_address,            clr_config_net_ipv4_address,          },
	{ "lxc.net.ipv4.gateway",          set_config_net_ipv4_gateway,            get_config_net_ipv4_gateway,            clr_config_net_ipv4_gateway,          },
	{ "lxc.net.ipv6.address",          set_config_net_ipv6_address,            get_config_net_ipv6_address,            clr_config_net_ipv6_address,          },
	{ "lxc.net.ipv6.gateway",          set_config_net_ipv6_gateway,            get_config_net_ipv6_gateway,            clr_config_net_ipv6_gateway,          },
	{ "lxc.net.link",                  set_config_net_link,                    get_config_net_link,                    clr_config_net_link,                  },
	{ "lxc.net.l2proxy",               set_config_net_l2proxy,                 get_config_net_l2proxy,                 clr_config_net_l2proxy,               },
	{ "lxc.net.macvlan.mode",          set_config_net_macvlan_mode,            get_config_net_macvlan_mode,            clr_config_net_macvlan_mode,          },
	{ "lxc.net.ipvlan.mode",           set_config_net_ipvlan_mode,             get_config_net_ipvlan_mode,             clr_config_net_ipvlan_mode,           },
	{ "lxc.net.ipvlan.isolation",      set_config_net_ipvlan_isolation,        get_config_net_ipvlan_isolation,        clr_config_net_ipvlan_isolation,      },
	{ "lxc.net.mtu",                   set_config_net_mtu,                     get_config_net_mtu,                     clr_config_net_mtu,                   },
	{ "lxc.net.name",                  set_config_net_name,                    get_config_net_name,                    clr_config_net_name,                  },
	{ "lxc.net.script.down",           set_config_net_script_down,             get_config_net_script_down,             clr_config_net_script_down,           },
	{ "lxc.net.script.up",             set_config_net_script_up,               get_config_net_script_up,               clr_config_net_script_up,             },
	{ "lxc.net.type",                  set_config_net_type,                    get_config_net_type,                    clr_config_net_type,                  },
	{ "lxc.net.vlan.id",               set_config_net_vlan_id,                 get_config_net_vlan_id,                 clr_config_net_vlan_id,               },
	{ "lxc.net.veth.mode",             set_config_net_veth_mode,               get_config_net_veth_mode,               clr_config_net_veth_mode,             },
	{ "lxc.net.veth.pair",             set_config_net_veth_pair,               get_config_net_veth_pair,               clr_config_net_veth_pair,             },
	{ "lxc.net.veth.ipv4.route",       set_config_net_veth_ipv4_route,         get_config_net_veth_ipv4_route,         clr_config_net_veth_ipv4_route,       },
	{ "lxc.net.veth.ipv6.route",       set_config_net_veth_ipv6_route,         get_config_net_veth_ipv6_route,         clr_config_net_veth_ipv6_route,       },
	{ "lxc.net.",                      set_config_net_nic,                     get_config_net_nic,                     clr_config_net_nic,                   },
	{ "lxc.net",                       set_config_net,                         get_config_net,                         clr_config_net,                       },
	{ "lxc.no_new_privs",	           set_config_no_new_privs,                get_config_no_new_privs,                clr_config_no_new_privs,              },
	{ "lxc.prlimit",                   set_config_prlimit,                     get_config_prlimit,                     clr_config_prlimit,                   },
	{ "lxc.pty.max",                   set_config_pty_max,                     get_config_pty_max,                     clr_config_pty_max,                   },
	{ "lxc.rootfs.managed",            set_config_rootfs_managed,              get_config_rootfs_managed,              clr_config_rootfs_managed,            },
	{ "lxc.rootfs.mount",              set_config_rootfs_mount,                get_config_rootfs_mount,                clr_config_rootfs_mount,              },
	{ "lxc.rootfs.options",            set_config_rootfs_options,              get_config_rootfs_options,              clr_config_rootfs_options,            },
	{ "lxc.rootfs.path",               set_config_rootfs_path,                 get_config_rootfs_path,                 clr_config_rootfs_path,               },
	{ "lxc.seccomp.allow_nesting",     set_config_seccomp_allow_nesting,       get_config_seccomp_allow_nesting,       clr_config_seccomp_allow_nesting,     },
	{ "lxc.seccomp.notify.cookie",     set_config_seccomp_notify_cookie,       get_config_seccomp_notify_cookie,       clr_config_seccomp_notify_cookie,     },
	{ "lxc.seccomp.notify.proxy",      set_config_seccomp_notify_proxy,        get_config_seccomp_notify_proxy,        clr_config_seccomp_notify_proxy,      },
	{ "lxc.seccomp.profile",           set_config_seccomp_profile,             get_config_seccomp_profile,             clr_config_seccomp_profile,           },
	{ "lxc.selinux.context.keyring",   set_config_selinux_context_keyring,     get_config_selinux_context_keyring,     clr_config_selinux_context_keyring    },
	{ "lxc.selinux.context",           set_config_selinux_context,             get_config_selinux_context,             clr_config_selinux_context,           },
	{ "lxc.signal.halt",               set_config_signal_halt,                 get_config_signal_halt,                 clr_config_signal_halt,               },
	{ "lxc.signal.reboot",             set_config_signal_reboot,               get_config_signal_reboot,               clr_config_signal_reboot,             },
	{ "lxc.signal.stop",               set_config_signal_stop,                 get_config_signal_stop,                 clr_config_signal_stop,               },
	{ "lxc.start.auto",                set_config_start,                       get_config_start,                       clr_config_start,                     },
	{ "lxc.start.delay",               set_config_start,                       get_config_start,                       clr_config_start,                     },
	{ "lxc.start.order",               set_config_start,                       get_config_start,                       clr_config_start,                     },
	{ "lxc.tty.dir",                   set_config_tty_dir,                     get_config_tty_dir,                     clr_config_tty_dir,                   },
	{ "lxc.tty.max",                   set_config_tty_max,                     get_config_tty_max,                     clr_config_tty_max,                   },
	{ "lxc.uts.name",                  set_config_uts_name,                    get_config_uts_name,                    clr_config_uts_name,                  },
	{ "lxc.sysctl",                    set_config_sysctl,                      get_config_sysctl,                      clr_config_sysctl,                    },
	{ "lxc.proc",                      set_config_proc,                        get_config_proc,                        clr_config_proc,                      },
};

static const size_t config_jump_table_size = sizeof(config_jump_table) / sizeof(struct lxc_config_t);

struct lxc_config_t *lxc_get_config_exact(const char *key)
{
	size_t i;

	for (i = 0; i < config_jump_table_size; i++)
		if (!strcmp(config_jump_table[i].name, key))
			return &config_jump_table[i];

	return NULL;
}


struct lxc_config_t *lxc_get_config(const char *key)
{
	size_t i;

	for (i = 0; i < config_jump_table_size; i++)
		if (!strncmp(config_jump_table[i].name, key, strlen(config_jump_table[i].name)))
			return &config_jump_table[i];

	return NULL;
}

static int set_config_net(const char *key, const char *value,
			  struct lxc_conf *lxc_conf, void *data)
{
	if (!lxc_config_value_empty(value)) {
		ERROR("lxc.net must not have a value");
		return -1;
	}

	return clr_config_net(key, lxc_conf, data);
}

static int set_config_net_type(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_type(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	if (strcmp(value, "veth") == 0) {
		netdev->type = LXC_NET_VETH;
		lxc_list_init(&netdev->priv.veth_attr.ipv4_routes);
		lxc_list_init(&netdev->priv.veth_attr.ipv6_routes);
		if (!lxc_veth_flag_to_mode(netdev->priv.veth_attr.mode))
			lxc_veth_mode_to_flag(&netdev->priv.veth_attr.mode, "bridge");
	} else if (strcmp(value, "macvlan") == 0) {
		netdev->type = LXC_NET_MACVLAN;
		if (!lxc_macvlan_flag_to_mode(netdev->priv.veth_attr.mode))
			lxc_macvlan_mode_to_flag(&netdev->priv.macvlan_attr.mode, "private");
	} else if (strcmp(value, "ipvlan") == 0) {
		netdev->type = LXC_NET_IPVLAN;
		if (!lxc_ipvlan_flag_to_mode(netdev->priv.ipvlan_attr.mode))
			lxc_ipvlan_mode_to_flag(&netdev->priv.ipvlan_attr.mode, "l3");
		if (!lxc_ipvlan_flag_to_isolation(netdev->priv.ipvlan_attr.isolation))
			lxc_ipvlan_isolation_to_flag(&netdev->priv.ipvlan_attr.isolation, "bridge");
	} else if (strcmp(value, "vlan") == 0) {
		netdev->type = LXC_NET_VLAN;
	} else if (strcmp(value, "phys") == 0) {
		netdev->type = LXC_NET_PHYS;
	} else if (strcmp(value, "empty") == 0) {
		netdev->type = LXC_NET_EMPTY;
	} else if (strcmp(value, "none") == 0) {
		netdev->type = LXC_NET_NONE;
	} else {
		return log_error(-1, "Invalid network type %s", value);
	}

	return 0;
}

static int set_config_net_flags(const char *key, const char *value,
				struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_flags(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->flags |= IFF_UP;

	return 0;
}

static int create_matched_ifnames(const char *value, struct lxc_conf *lxc_conf,
				  struct lxc_netdev *netdev)
{
	call_cleaner(netns_freeifaddrs) struct netns_ifaddrs *ifaddr = NULL;
	struct netns_ifaddrs *ifa;
	int n;
	int ret = 0;
	const char *type_key = "lxc.net.type";
	const char *link_key = "lxc.net.link";
	const char *tmpvalue = "phys";

	if (netns_getifaddrs(&ifaddr, -1, &(bool){false}) < 0)
		return log_error_errno(-1, errno, "Failed to get network interfaces");

	for (ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
		if (!ifa->ifa_addr)
			continue;

		if (ifa->ifa_addr->sa_family != AF_PACKET)
			continue;

		if (!strncmp(value, ifa->ifa_name, strlen(value) - 1)) {
			ret = set_config_net_type(type_key, tmpvalue, lxc_conf,
						  netdev);
			if (!ret) {
				ret = set_config_net_link(
				    link_key, ifa->ifa_name, lxc_conf, netdev);
				if (ret) {
					ERROR("Failed to create matched ifnames");
					break;
				}
			} else {
				ERROR("Failed to create matched ifnames");
				break;
			}
		}
	}

	return ret;
}

static int set_config_net_link(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	int ret = 0;

	if (lxc_config_value_empty(value))
		return clr_config_net_link(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	if (value[strlen(value) - 1] == '+' && netdev->type == LXC_NET_PHYS)
		ret = create_matched_ifnames(value, lxc_conf, netdev);
	else
		ret = network_ifname(netdev->link, value, sizeof(netdev->link));

	return ret;
}

static int set_config_net_l2proxy(const char *key, const char *value,
				     struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	unsigned int val = 0;
	int ret;

	if (lxc_config_value_empty(value))
		return clr_config_net_l2proxy(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	ret = lxc_safe_uint(value, &val);
	if (ret < 0)
		return ret_errno(ret);

	switch (val) {
	case 0:
		netdev->l2proxy = false;
		return 0;
	case 1:
		netdev->l2proxy = true;
		return 0;
	}

	return ret_errno(EINVAL);
}

static int set_config_net_name(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_name(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return network_ifname(netdev->name, value, sizeof(netdev->name));
}


static int set_config_net_veth_mode(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_veth_mode(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return lxc_veth_mode_to_flag(&netdev->priv.veth_attr.mode, value);
}

static int set_config_net_veth_pair(const char *key, const char *value,
				    struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_veth_pair(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return network_ifname(netdev->priv.veth_attr.pair, value,
			      sizeof(netdev->priv.veth_attr.pair));
}

static int set_config_net_macvlan_mode(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_macvlan_mode(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return lxc_macvlan_mode_to_flag(&netdev->priv.macvlan_attr.mode, value);
}

static int set_config_net_ipvlan_mode(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_ipvlan_mode(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_IPVLAN)
		return log_error_errno(-EINVAL,
				       EINVAL, "Invalid ipvlan mode \"%s\", can only be used with ipvlan network",
				       value);

	return lxc_ipvlan_mode_to_flag(&netdev->priv.ipvlan_attr.mode, value);
}

static int set_config_net_ipvlan_isolation(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_ipvlan_isolation(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_IPVLAN)
		return log_error_errno(-EINVAL,
				       EINVAL, "Invalid ipvlan isolation \"%s\", can only be used with ipvlan network",
				       value);

	return lxc_ipvlan_isolation_to_flag(&netdev->priv.ipvlan_attr.isolation, value);
}

static int set_config_net_hwaddr(const char *key, const char *value,
				 struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	char *new_value;

	if (lxc_config_value_empty(value))
		return clr_config_net_hwaddr(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	new_value = strdup(value);
	if (!new_value)
		return ret_errno(ENOMEM);

	rand_complete_hwaddr(new_value);

	if (lxc_config_value_empty(new_value)) {
		free(new_value);
		netdev->hwaddr = NULL;
		return 0;
	}

	netdev->hwaddr = new_value;

	return 0;
}

static int set_config_net_vlan_id(const char *key, const char *value,
				  struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_vlan_id(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	ret = get_u16(&netdev->priv.vlan_attr.vid, value, 0);
	if (ret < 0)
		return ret;

	return 0;
}

static int set_config_net_mtu(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_mtu(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return set_config_string_item(&netdev->mtu, value);
}

static int set_config_net_ipv4_address(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *addr = NULL;
	__do_free struct lxc_inetdev *inetdev = NULL;
	__do_free struct lxc_list *list = NULL;
	int ret;
	struct lxc_netdev *netdev = data;
	char *cursor, *slash;
	char *bcast = NULL, *prefix = NULL;

	if (lxc_config_value_empty(value))
		return clr_config_net_ipv4_address(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	inetdev = malloc(sizeof(*inetdev));
	if (!inetdev)
		return ret_errno(ENOMEM);
	memset(inetdev, 0, sizeof(*inetdev));

	list = malloc(sizeof(*list));
	if (!list)
		return ret_errno(ENOMEM);

	lxc_list_init(list);

	addr = strdup(value);
	if (!addr)
		return ret_errno(ENOMEM);

	cursor = strstr(addr, " ");
	if (cursor) {
		*cursor = '\0';
		bcast = cursor + 1;
	}

	slash = strstr(addr, "/");
	if (slash) {
		*slash = '\0';
		prefix = slash + 1;
	}

	ret = inet_pton(AF_INET, addr, &inetdev->addr);
	if (!ret || ret < 0)
		return log_error_errno(-1, errno, "Invalid ipv4 address \"%s\"", value);

	if (bcast) {
		ret = inet_pton(AF_INET, bcast, &inetdev->bcast);
		if (!ret || ret < 0)
			return log_error_errno(-1, errno, "Invalid ipv4 broadcast address \"%s\"", value);

	}

	/* No prefix specified, determine it from the network class. */
	if (prefix) {
		ret = lxc_safe_uint(prefix, &inetdev->prefix);
		if (ret < 0)
			return ret;
	} else {
		inetdev->prefix = config_ip_prefix(&inetdev->addr);
	}

	/* If no broadcast address, let compute one from the
	 * prefix and address.
	 */
	if (!bcast) {
		inetdev->bcast.s_addr = inetdev->addr.s_addr;
		inetdev->bcast.s_addr |= htonl(INADDR_BROADCAST >> inetdev->prefix);
	}

	list->elem = inetdev;
	lxc_list_add_tail(&netdev->ipv4, list);
	move_ptr(inetdev);
	move_ptr(list);

	return 0;
}

static int set_config_net_ipv4_gateway(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_ipv4_gateway(key, lxc_conf, data);

	if (!netdev)
		return -1;

	free(netdev->ipv4_gateway);

	if (strcmp(value, "auto") == 0) {
		netdev->ipv4_gateway = NULL;
		netdev->ipv4_gateway_auto = true;
	} else if (strcmp(value, "dev") == 0) {
		netdev->ipv4_gateway = NULL;
		netdev->ipv4_gateway_auto = false;
		netdev->ipv4_gateway_dev = true;
	} else {
		__do_free struct in_addr *gw = NULL;
		int ret;

		gw = malloc(sizeof(*gw));
		if (!gw)
			return ret_errno(ENOMEM);

		ret = inet_pton(AF_INET, value, gw);
		if (!ret || ret < 0)
			return log_error_errno(-1, errno, "Invalid ipv4 gateway address \"%s\"", value);

		netdev->ipv4_gateway = move_ptr(gw);
		netdev->ipv4_gateway_auto = false;
	}

	return 0;
}

static int set_config_net_veth_ipv4_route(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *valdup = NULL;
	__do_free struct lxc_inetdev *inetdev = NULL;
	__do_free struct lxc_list *list = NULL;
	int ret;
	char *netmask, *slash;
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_veth_ipv4_route(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return log_error_errno(-EINVAL,
				       EINVAL, "Invalid ipv4 route \"%s\", can only be used with veth network",
				       value);

	inetdev = malloc(sizeof(*inetdev));
	if (!inetdev)
		return ret_errno(ENOMEM);
	memset(inetdev, 0, sizeof(*inetdev));

	list = malloc(sizeof(*list));
	if (!list)
		return ret_errno(ENOMEM);

	lxc_list_init(list);
	list->elem = inetdev;

	valdup = strdup(value);
	if (!valdup)
		return ret_errno(ENOMEM);

	slash = strchr(valdup, '/');
	if (!slash)
		return ret_errno(EINVAL);

	*slash = '\0';
	slash++;
	if (*slash == '\0')
		return ret_errno(EINVAL);

	netmask = slash;

	ret = lxc_safe_uint(netmask, &inetdev->prefix);
	if (ret < 0 || inetdev->prefix > 32)
		return ret_errno(EINVAL);

	ret = inet_pton(AF_INET, valdup, &inetdev->addr);
	if (!ret || ret < 0)
		return ret_errno(EINVAL);

	lxc_list_add_tail(&netdev->priv.veth_attr.ipv4_routes, list);
	move_ptr(inetdev);
	move_ptr(list);

	return 0;
}

static int set_config_net_ipv6_address(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *valdup = NULL;
	__do_free struct lxc_inet6dev *inet6dev = NULL;
	__do_free struct lxc_list *list = NULL;
	int ret;
	struct lxc_netdev *netdev = data;
	char *slash, *netmask;

	if (lxc_config_value_empty(value))
		return clr_config_net_ipv6_address(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	inet6dev = malloc(sizeof(*inet6dev));
	if (!inet6dev)
		return ret_errno(ENOMEM);
	memset(inet6dev, 0, sizeof(*inet6dev));

	list = malloc(sizeof(*list));
	if (!list)
		return ret_errno(ENOMEM);

	lxc_list_init(list);

	valdup = strdup(value);
	if (!valdup)
		return ret_errno(ENOMEM);

	inet6dev->prefix = 64;
	slash = strstr(valdup, "/");
	if (slash) {
		*slash = '\0';
		netmask = slash + 1;

		ret = lxc_safe_uint(netmask, &inet6dev->prefix);
		if (ret < 0)
			return ret;
	}

	ret = inet_pton(AF_INET6, valdup, &inet6dev->addr);
	if (!ret || ret < 0)
		return log_error_errno(-EINVAL, EINVAL, "Invalid ipv6 address \"%s\"", valdup);

	list->elem = inet6dev;
	lxc_list_add_tail(&netdev->ipv6, list);
	move_ptr(inet6dev);
	move_ptr(list);

	return 0;
}

static int set_config_net_ipv6_gateway(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_ipv6_gateway(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	free(netdev->ipv6_gateway);

	if (strcmp(value, "auto") == 0) {
		netdev->ipv6_gateway = NULL;
		netdev->ipv6_gateway_auto = true;
	} else if (strcmp(value, "dev") == 0) {
		netdev->ipv6_gateway = NULL;
		netdev->ipv6_gateway_auto = false;
		netdev->ipv6_gateway_dev = true;
	} else {
		int ret;
		__do_free struct in6_addr *gw = NULL;

		gw = malloc(sizeof(*gw));
		if (!gw)
			return ret_errno(ENOMEM);

		ret = inet_pton(AF_INET6, value, gw);
		if (!ret || ret < 0)
			return log_error_errno(-EINVAL, EINVAL,
					       "Invalid ipv6 gateway address \"%s\"", value);

		netdev->ipv6_gateway = move_ptr(gw);
		netdev->ipv6_gateway_auto = false;
	}

	return 0;
}

static int set_config_net_veth_ipv6_route(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *valdup = NULL;
	__do_free struct lxc_inet6dev *inet6dev = NULL;
	__do_free struct lxc_list *list = NULL;
	int ret;
	char *netmask, *slash;
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_veth_ipv6_route(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return log_error_errno(-EINVAL,
				       EINVAL, "Invalid ipv6 route \"%s\", can only be used with veth network",
				       value);

	inet6dev = malloc(sizeof(*inet6dev));
	if (!inet6dev)
		return ret_errno(ENOMEM);
	memset(inet6dev, 0, sizeof(*inet6dev));

	list = malloc(sizeof(*list));
	if (!list)
		return ret_errno(ENOMEM);

	lxc_list_init(list);

	valdup = strdup(value);
	if (!valdup)
		return -1;

	slash = strchr(valdup, '/');
	if (!slash)
		return ret_errno(EINVAL);

	*slash = '\0';
	slash++;
	if (*slash == '\0')
		return ret_errno(EINVAL);

	netmask = slash;

	ret = lxc_safe_uint(netmask, &inet6dev->prefix);
	if (ret < 0 || inet6dev->prefix > 128)
		return ret_errno(EINVAL);

	ret = inet_pton(AF_INET6, valdup, &inet6dev->addr);
	if (!ret || ret < 0)
		return ret_errno(EINVAL);

	list->elem = inet6dev;
	lxc_list_add_tail(&netdev->priv.veth_attr.ipv6_routes, list);
	move_ptr(inet6dev);
	move_ptr(list);

	return 0;
}

static int set_config_net_script_up(const char *key, const char *value,
				    struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_script_up(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return set_config_string_item(&netdev->upscript, value);
}

static int set_config_net_script_down(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (lxc_config_value_empty(value))
		return clr_config_net_script_down(key, lxc_conf, data);

	if (!netdev)
		return ret_errno(EINVAL);

	return set_config_string_item(&netdev->downscript, value);
}

static int add_hook(struct lxc_conf *lxc_conf, int which, __owns char *hook)
{
	__do_free char *val = hook;
	struct lxc_list *hooklist;

	hooklist = malloc(sizeof(*hooklist));
	if (!hooklist)
		return ret_errno(ENOMEM);

	hooklist->elem = move_ptr(val);
	lxc_list_add_tail(&lxc_conf->hooks[which], hooklist);

	return 0;
}

static int set_config_seccomp_allow_nesting(const char *key, const char *value,
					    struct lxc_conf *lxc_conf, void *data)
{
#ifdef HAVE_SECCOMP
	if (lxc_config_value_empty(value))
		return clr_config_seccomp_allow_nesting(key, lxc_conf, NULL);

	if (lxc_safe_uint(value, &lxc_conf->seccomp.allow_nesting) < 0)
		return -1;

	if (lxc_conf->seccomp.allow_nesting > 1)
		return ret_set_errno(-1, EINVAL);

	return 0;
#else
	errno = ENOSYS;
	return -1;
#endif
}

static int set_config_seccomp_notify_cookie(const char *key, const char *value,
					    struct lxc_conf *lxc_conf, void *data)
{
#ifdef HAVE_SECCOMP_NOTIFY
	return set_config_string_item(&lxc_conf->seccomp.notifier.cookie, value);
#else
	return ret_set_errno(-1, ENOSYS);
#endif
}

static int set_config_seccomp_notify_proxy(const char *key, const char *value,
					   struct lxc_conf *lxc_conf, void *data)
{
#ifdef HAVE_SECCOMP_NOTIFY
	const char *offset;

	if (lxc_config_value_empty(value))
		return clr_config_seccomp_notify_proxy(key, lxc_conf, NULL);

	if (strncmp(value, "unix:", 5) != 0)
		return ret_set_errno(-1, EINVAL);

	offset = value + 5;
	if (lxc_unix_sockaddr(&lxc_conf->seccomp.notifier.proxy_addr, offset) < 0)
		return -1;

	return 0;
#else
	return ret_set_errno(-1, ENOSYS);
#endif
}

static int set_config_seccomp_profile(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->seccomp.seccomp, value);
}

static int set_config_execute_cmd(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->execute_cmd, value);
}

static int set_config_init_cmd(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->init_cmd, value);
}

static int set_config_init_cwd(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->init_cwd, value);
}

static int set_config_init_uid(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	unsigned int init_uid;

	if (lxc_config_value_empty(value)) {
		lxc_conf->init_uid = 0;
		return 0;
	}

	if (lxc_safe_uint(value, &init_uid) < 0)
		return -1;

	lxc_conf->init_uid = init_uid;

	return 0;
}

static int set_config_init_gid(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	unsigned int init_gid;

	if (lxc_config_value_empty(value)) {
		lxc_conf->init_gid = 0;
		return 0;
	}

	if (lxc_safe_uint(value, &init_gid) < 0)
		return -1;

	lxc_conf->init_gid = init_gid;

	return 0;
}

static int set_config_hooks(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *copy = NULL;

	if (lxc_config_value_empty(value))
		return lxc_clear_hooks(lxc_conf, key);

	if (strcmp(key + 4, "hook") == 0)
		return log_error_errno(-EINVAL, EINVAL, "lxc.hook must not have a value");

	copy = strdup(value);
	if (!copy)
		return ret_errno(ENOMEM);

	if (strcmp(key + 9, "pre-start") == 0)
		return add_hook(lxc_conf, LXCHOOK_PRESTART, move_ptr(copy));
	else if (strcmp(key + 9, "start-host") == 0)
		return add_hook(lxc_conf, LXCHOOK_START_HOST, move_ptr(copy));
	else if (strcmp(key + 9, "pre-mount") == 0)
		return add_hook(lxc_conf, LXCHOOK_PREMOUNT, move_ptr(copy));
	else if (strcmp(key + 9, "autodev") == 0)
		return add_hook(lxc_conf, LXCHOOK_AUTODEV, move_ptr(copy));
	else if (strcmp(key + 9, "mount") == 0)
		return add_hook(lxc_conf, LXCHOOK_MOUNT, move_ptr(copy));
	else if (strcmp(key + 9, "start") == 0)
		return add_hook(lxc_conf, LXCHOOK_START, move_ptr(copy));
	else if (strcmp(key + 9, "stop") == 0)
		return add_hook(lxc_conf, LXCHOOK_STOP, move_ptr(copy));
	else if (strcmp(key + 9, "post-stop") == 0)
		return add_hook(lxc_conf, LXCHOOK_POSTSTOP, move_ptr(copy));
	else if (strcmp(key + 9, "clone") == 0)
		return add_hook(lxc_conf, LXCHOOK_CLONE, move_ptr(copy));
	else if (strcmp(key + 9, "destroy") == 0)
		return add_hook(lxc_conf, LXCHOOK_DESTROY, move_ptr(copy));

	return ret_errno(EINVAL);
}

static int set_config_hooks_version(const char *key, const char *value,
				    struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	unsigned int tmp;

	if (lxc_config_value_empty(value))
		return clr_config_hooks_version(key, lxc_conf, NULL);

	ret = lxc_safe_uint(value, &tmp);
	if (ret < 0)
		return -1;

	if (tmp > 1)
		return log_error_errno(-EINVAL,
				       EINVAL, "Invalid hook version specified. Currently only 0 (legacy) and 1 are supported");

	lxc_conf->hooks_version = tmp;

	return 0;
}

static int set_config_personality(const char *key, const char *value,
				  struct lxc_conf *lxc_conf, void *data)
{
	signed long personality;

	personality = lxc_config_parse_arch(value);
	if (personality >= 0)
		lxc_conf->personality = personality;
	else
		WARN("Unsupported personality \"%s\"", value);

	return 0;
}

static int set_config_pty_max(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	unsigned int max = 0;

	if (lxc_config_value_empty(value)) {
		lxc_conf->pty_max = 0;
		return 0;
	}

	ret = lxc_safe_uint(value, &max);
	if (ret < 0)
		return ret_errno(EINVAL);

	lxc_conf->pty_max = max;

	return 0;
}

/* We only need to check whether the first byte of the key after the lxc.start.
 * prefix matches our expectations since they fortunately all start with a
 * different letter. If anything was wrong with the key we would have already
 * noticed when the callback was called.
 */
static int set_config_start(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	bool is_empty;

	is_empty = lxc_config_value_empty(value);

	if (*(key + 10) == 'a') { /* lxc.start.auto */
		if (is_empty) {
			lxc_conf->start_auto = 0;
			return 0;
		}

		ret = lxc_safe_uint(value, &lxc_conf->start_auto);
		if (ret)
			return ret;

		if (lxc_conf->start_auto > 1)
			return ret_errno(EINVAL);

		return 0;
	} else if (*(key + 10) == 'd') { /* lxc.start.delay */
		if (is_empty) {
			lxc_conf->start_delay = 0;
			return 0;
		}

		return lxc_safe_uint(value, &lxc_conf->start_delay);
	} else if (*(key + 10) == 'o') { /* lxc.start.order */
		if (is_empty) {
			lxc_conf->start_order = 0;
			return 0;
		}

		return lxc_safe_int(value, &lxc_conf->start_order);
	}

	return ret_errno(EINVAL);
}

static int set_config_monitor(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	if (lxc_config_value_empty(value)) {
		lxc_conf->monitor_unshare = 0;
		return 0;
	}

	if (strcmp(key + 12, "unshare") == 0)
		return lxc_safe_uint(value, &lxc_conf->monitor_unshare);

	return ret_errno(EINVAL);
}

static int set_config_monitor_signal_pdeath(const char *key, const char *value,
					    struct lxc_conf *lxc_conf, void *data)
{
	if (lxc_config_value_empty(value)) {
		lxc_conf->monitor_signal_pdeath = 0;
		return 0;
	}

	if (strcmp(key + 12, "signal.pdeath") == 0) {
		int sig_n;

		sig_n = sig_parse(value);
		if (sig_n < 0)
			return ret_errno(EINVAL);

		lxc_conf->monitor_signal_pdeath = sig_n;
		return 0;
	}

	return ret_errno(EINVAL);
}

static int set_config_group(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *groups = NULL;
	char *token;

	if (lxc_config_value_empty(value))
		return lxc_clear_groups(lxc_conf);

	groups = strdup(value);
	if (!groups)
		return ret_errno(ENOMEM);

	/* In case several groups are specified in a single line split these
	 * groups in a single element for the list.
	 */
	lxc_iterate_parts(token, groups, " \t") {
		__do_free struct lxc_list *grouplist = NULL;

		grouplist = malloc(sizeof(*grouplist));
		if (!grouplist)
			return ret_errno(ENOMEM);

		grouplist->elem = strdup(token);
		if (!grouplist->elem)
			return ret_errno(ENOMEM);

		lxc_list_add_tail(&lxc_conf->groups, move_ptr(grouplist));
	}

	return 0;
}

static int set_config_environment(const char *key, const char *value,
				  struct lxc_conf *lxc_conf, void *data)
{
	__do_free struct lxc_list *list_item = NULL;

	if (lxc_config_value_empty(value))
		return lxc_clear_environment(lxc_conf);

	list_item = malloc(sizeof(*list_item));
	if (!list_item)
		return ret_errno(ENOMEM);

	if (!strchr(value, '=')) {
		const char *env_val;
		const char *env_key = value;
		const char *env_var[3] = {0};

		env_val = getenv(env_key);
		if (!env_val)
			return ret_errno(ENOENT);

		env_var[0] = env_key;
		env_var[1] = env_val;
		list_item->elem = lxc_string_join("=", env_var, false);
	} else {
		list_item->elem = strdup(value);
	}

	if (!list_item->elem)
		return ret_errno(ENOMEM);

	lxc_list_add_tail(&lxc_conf->environment, move_ptr(list_item));

	return 0;
}

static int set_config_tty_max(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	unsigned int nbtty = 0;

	if (lxc_config_value_empty(value)) {
		lxc_conf->ttys.max = 0;
		return 0;
	}

	ret = lxc_safe_uint(value, &nbtty);
	if (ret < 0)
		return ret;

	lxc_conf->ttys.max = nbtty;

	return 0;
}

static int set_config_tty_dir(const char *key, const char *value,
			     struct lxc_conf *lxc_conf, void *data)
{
	return set_config_string_item_max(&lxc_conf->ttys.dir, value,
					  NAME_MAX + 1);
}

static int set_config_apparmor_profile(const char *key, const char *value,
				       struct lxc_conf *lxc_conf, void *data)
{
	return set_config_string_item(&lxc_conf->lsm_aa_profile, value);
}

static int set_config_apparmor_allow_incomplete(const char *key,
						const char *value,
						struct lxc_conf *lxc_conf,
						void *data)
{
	int ret;

	if (lxc_config_value_empty(value)) {
		lxc_conf->lsm_aa_allow_incomplete = 0;
		return 0;
	}

	ret = lxc_safe_uint(value, &lxc_conf->lsm_aa_allow_incomplete);
	if (ret)
		return ret;

	if (lxc_conf->lsm_aa_allow_incomplete > 1)
		return ret_errno(EINVAL);

	return 0;
}

static int set_config_apparmor_allow_nesting(const char *key,
					     const char *value,
					     struct lxc_conf *lxc_conf,
					     void *data)
{
	int ret;

	if (lxc_config_value_empty(value))
		return clr_config_apparmor_allow_nesting(key, lxc_conf, NULL);

	ret = lxc_safe_uint(value, &lxc_conf->lsm_aa_allow_nesting);
	if (ret)
		return ret;

	if (lxc_conf->lsm_aa_allow_nesting > 1)
		return ret_errno(EINVAL);

	return 0;
}

static int set_config_apparmor_raw(const char *key,
				   const char *value,
				   struct lxc_conf *lxc_conf,
				   void *data)
{
	__do_free char *elem = NULL;
	__do_free struct lxc_list *list = NULL;

	if (lxc_config_value_empty(value))
		return lxc_clear_apparmor_raw(lxc_conf);

	list = malloc(sizeof(*list));
	if (!list)
		return ret_errno(ENOMEM);

	elem = strdup(value);
	if (!elem)
		return ret_errno(ENOMEM);

	list->elem = move_ptr(elem);
	lxc_list_add_tail(&lxc_conf->lsm_aa_raw, move_ptr(list));

	return 0;
}

static int set_config_selinux_context(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	return set_config_string_item(&lxc_conf->lsm_se_context, value);
}

static int set_config_selinux_context_keyring(const char *key, const char *value,
					      struct lxc_conf *lxc_conf, void *data)
{
	return set_config_string_item(&lxc_conf->lsm_se_keyring_context, value);
}

static int set_config_keyring_session(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	return set_config_bool_item(&lxc_conf->keyring_disable_session, value, false);
}

static int set_config_log_file(const char *key, const char *value,
			      struct lxc_conf *c, void *data)
{
	int ret;

	if (lxc_config_value_empty(value)) {
		free_disarm(c->logfile);
		return 0;
	}

	/*
	 * Store these values in the lxc_conf, and then try to set for actual
	 * current logging.
	 */
	ret = set_config_path_item(&c->logfile, value);
	if (ret == 0)
		ret = lxc_log_set_file(&c->logfd, c->logfile);

	return ret;
}

static int set_config_log_level(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	int newlevel;

	if (lxc_config_value_empty(value)) {
		lxc_conf->loglevel = LXC_LOG_LEVEL_NOTSET;
		return 0;
	}

	if (value[0] >= '0' && value[0] <= '9') {
		int ret;

		ret = lxc_safe_int(value, &newlevel);
		if (ret)
			return ret_errno(EINVAL);
	} else {
		newlevel = lxc_log_priority_to_int(value);
	}

	/*
	 * Store these values in the lxc_conf, and then try to set for actual
	 * current logging.
	 */
	lxc_conf->loglevel = newlevel;

	return lxc_log_set_level(&lxc_conf->loglevel, newlevel);
}

static int set_config_autodev(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	int ret;

	if (lxc_config_value_empty(value)) {
		lxc_conf->autodev = 0;
		return 0;
	}

	ret = lxc_safe_uint(value, &lxc_conf->autodev);
	if (ret)
		return ret_errno(EINVAL);

	if (lxc_conf->autodev > 1)
		return ret_errno(EINVAL);

	return 0;
}

static int set_config_autodev_tmpfs_size(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	if (lxc_config_value_empty(value)) {
		lxc_conf->autodevtmpfssize = 500000;
		return 0;
	}

	if (lxc_safe_int(value, &lxc_conf->autodevtmpfssize) < 0)
		lxc_conf->autodevtmpfssize = 500000;

	return 0;
}

static int set_config_signal_halt(const char *key, const char *value,
				 struct lxc_conf *lxc_conf, void *data)
{
	int sig_n;

	if (lxc_config_value_empty(value)) {
		lxc_conf->haltsignal = 0;
		return 0;
	}

	sig_n = sig_parse(value);
	if (sig_n < 0)
		return ret_errno(EINVAL);

	lxc_conf->haltsignal = sig_n;

	return 0;
}

static int set_config_signal_reboot(const char *key, const char *value,
				   struct lxc_conf *lxc_conf, void *data)
{
	int sig_n;

	if (lxc_config_value_empty(value)) {
		lxc_conf->rebootsignal = 0;
		return 0;
	}

	sig_n = sig_parse(value);
	if (sig_n < 0)
		return ret_errno(EINVAL);

	lxc_conf->rebootsignal = sig_n;

	return 0;
}

static int set_config_signal_stop(const char *key, const char *value,
				 struct lxc_conf *lxc_conf, void *data)
{
	int sig_n;

	if (lxc_config_value_empty(value)) {
		lxc_conf->stopsignal = 0;
		return 0;
	}

	sig_n = sig_parse(value);
	if (sig_n < 0)
		return ret_errno(EINVAL);

	lxc_conf->stopsignal = sig_n;

	return 0;
}

static int __set_config_cgroup_controller(const char *key, const char *value,
					  struct lxc_conf *lxc_conf, int version)
{
	__do_free struct lxc_list *cglist = NULL;
	call_cleaner(free_lxc_cgroup) struct lxc_cgroup *cgelem = NULL;
	const char *subkey, *token;
	size_t token_len;

	if (lxc_config_value_empty(value))
		return lxc_clear_cgroups(lxc_conf, key, version);

	if (version == CGROUP2_SUPER_MAGIC) {
		token = "lxc.cgroup2.";
		token_len = 12;
	} else if (version == CGROUP_SUPER_MAGIC) {
		token = "lxc.cgroup.";
		token_len = 11;
	} else {
		return ret_errno(EINVAL);
	}

	if (strncmp(key, token, token_len) != 0)
		return ret_errno(EINVAL);

	subkey = key + token_len;
	if (*subkey == '\0')
		return ret_errno(EINVAL);

	cglist = malloc(sizeof(*cglist));
	if (!cglist)
		return ret_errno(ENOMEM);

	cgelem = malloc(sizeof(*cgelem));
	if (!cgelem)
		return ret_errno(ENOMEM);
	memset(cgelem, 0, sizeof(*cgelem));

	cgelem->subsystem = strdup(subkey);
	if (!cgelem->subsystem)
		return ret_errno(ENOMEM);

	cgelem->value = strdup(value);
	if (!cgelem->value)
		return ret_errno(ENOMEM);

	cgelem->version = version;

	lxc_list_add_elem(cglist, move_ptr(cgelem));

	if (version == CGROUP2_SUPER_MAGIC)
		lxc_list_add_tail(&lxc_conf->cgroup2, cglist);
	else
		lxc_list_add_tail(&lxc_conf->cgroup, cglist);
	move_ptr(cglist);

	return 0;
}

static int set_config_cgroup_controller(const char *key, const char *value,
					struct lxc_conf *lxc_conf, void *data)
{
	return __set_config_cgroup_controller(key, value, lxc_conf,
					      CGROUP_SUPER_MAGIC);
}

static int set_config_cgroup2_controller(const char *key, const char *value,
					 struct lxc_conf *lxc_conf, void *data)
{
	return __set_config_cgroup_controller(key, value, lxc_conf,
					      CGROUP2_SUPER_MAGIC);
}


static int set_config_cgroup_dir(const char *key, const char *value,
				 struct lxc_conf *lxc_conf, void *data)
{
	if (strcmp(key, "lxc.cgroup.dir") != 0)
		return ret_errno(EINVAL);

	if (lxc_config_value_empty(value))
		return clr_config_cgroup_dir(key, lxc_conf, NULL);

	return set_config_string_item(&lxc_conf->cgroup_meta.dir, value);
}

static int set_config_cgroup_relative(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	unsigned int converted;
	int ret;

	if (lxc_config_value_empty(value))
		return clr_config_cgroup_relative(key, lxc_conf, NULL);

	ret = lxc_safe_uint(value, &converted);
	if (ret)
		return ret;

	if (converted == 1) {
		lxc_conf->cgroup_meta.relative = true;
		return 0;
	}

	if (converted == 0) {
		lxc_conf->cgroup_meta.relative = false;
		return 0;
	}

	return ret_errno(EINVAL);
}

static bool parse_limit_value(const char **value, rlim_t *res)
{
	char *endptr = NULL;

	if (strncmp(*value, "unlimited", STRLITERALLEN("unlimited")) == 0) {
		*res = RLIM_INFINITY;
		*value += STRLITERALLEN("unlimited");
		return true;
	}

	errno = 0;
	*res = strtoull(*value, &endptr, 10);
	if (errno || !endptr)
		return false;

	*value = endptr;

	return true;
}

static int set_config_prlimit(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	__do_free struct lxc_list *limlist = NULL;
	call_cleaner(free_lxc_limit) struct lxc_limit *limelem = NULL;
	struct lxc_list *iter;
	struct rlimit limit;
	rlim_t limit_value;

	if (lxc_config_value_empty(value))
		return lxc_clear_limits(lxc_conf, key);

	if (strncmp(key, "lxc.prlimit.", STRLITERALLEN("lxc.prlimit.")) != 0)
		return ret_errno(EINVAL);

	key += STRLITERALLEN("lxc.prlimit.");

	/* soft limit comes first in the value */
	if (!parse_limit_value(&value, &limit_value))
		return ret_errno(EINVAL);

	limit.rlim_cur = limit_value;

	/* skip spaces and a colon */
	while (isspace(*value))
		++value;

	if (*value == ':')
		++value;
	else if (*value) /* any other character is an error here */
		return ret_errno(EINVAL);

	while (isspace(*value))
		++value;

	/* optional hard limit */
	if (*value) {
		if (!parse_limit_value(&value, &limit_value))
			return ret_errno(EINVAL);

		limit.rlim_max = limit_value;

		/* check for trailing garbage */
		while (isspace(*value))
			++value;

		if (*value)
			return ret_errno(EINVAL);
	} else {
		/* a single value sets both hard and soft limit */
		limit.rlim_max = limit.rlim_cur;
	}

	/* find existing list element */
	lxc_list_for_each(iter, &lxc_conf->limits) {
		limelem = iter->elem;
		if (!strcmp(key, limelem->resource)) {
			limelem->limit = limit;
			return 0;
		}
	}

	/* allocate list element */
	limlist = malloc(sizeof(*limlist));
	if (!limlist)
		return ret_errno(ENOMEM);

	limelem = malloc(sizeof(*limelem));
	if (!limelem)
		return ret_errno(ENOMEM);
	memset(limelem, 0, sizeof(*limelem));

	limelem->resource = strdup(key);
	if (!limelem->resource)
		return ret_errno(ENOMEM);

	limelem->limit = limit;
	lxc_list_add_elem(limlist, move_ptr(limelem));;
	lxc_list_add_tail(&lxc_conf->limits, move_ptr(limlist));

	return 0;
}

static int set_config_sysctl(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	__do_free struct lxc_list *sysctl_list = NULL;
	call_cleaner(free_lxc_sysctl) struct lxc_sysctl *sysctl_elem = NULL;
	struct lxc_list *iter;

	if (lxc_config_value_empty(value))
		return clr_config_sysctl(key, lxc_conf, NULL);

	if (strncmp(key, "lxc.sysctl.", STRLITERALLEN("lxc.sysctl.")) != 0)
		return -1;

	key += STRLITERALLEN("lxc.sysctl.");

	/* find existing list element */
	lxc_list_for_each(iter, &lxc_conf->sysctls) {
		__do_free char *replace_value = NULL;

		sysctl_elem = iter->elem;

		if (strcmp(key, sysctl_elem->key) != 0)
			continue;

		replace_value = strdup(value);
		if (!replace_value)
			return ret_errno(EINVAL);

		free(sysctl_elem->value);
		sysctl_elem->value = move_ptr(replace_value);

		return 0;
	}

	/* allocate list element */
	sysctl_list = malloc(sizeof(*sysctl_list));
	if (!sysctl_list)
		return ret_errno(ENOMEM);

	sysctl_elem = malloc(sizeof(*sysctl_elem));
	if (!sysctl_elem)
		return ret_errno(ENOMEM);
	memset(sysctl_elem, 0, sizeof(*sysctl_elem));

	sysctl_elem->key = strdup(key);
	if (!sysctl_elem->key)
		return ret_errno(ENOMEM);

	sysctl_elem->value = strdup(value);
	if (!sysctl_elem->value)
		return ret_errno(ENOMEM);

	lxc_list_add_elem(sysctl_list, move_ptr(sysctl_elem));
	lxc_list_add_tail(&lxc_conf->sysctls, move_ptr(sysctl_list));

	return 0;
}

static int set_config_proc(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	__do_free struct lxc_list *proclist = NULL;
	call_cleaner(free_lxc_proc) struct lxc_proc *procelem = NULL;
	const char *subkey;

	if (lxc_config_value_empty(value))
		return clr_config_proc(key, lxc_conf, NULL);

	if (strncmp(key, "lxc.proc.", STRLITERALLEN("lxc.proc.")) != 0)
		return -1;

	subkey = key + STRLITERALLEN("lxc.proc.");
	if (*subkey == '\0')
		return ret_errno(EINVAL);

	proclist = malloc(sizeof(*proclist));
	if (!proclist)
		return ret_errno(ENOMEM);

	procelem = malloc(sizeof(*procelem));
	if (!procelem)
		return ret_errno(ENOMEM);
	memset(procelem, 0, sizeof(*procelem));

	procelem->filename = strdup(subkey);
	if (!procelem->filename)
		return ret_errno(ENOMEM);

	procelem->value = strdup(value);
	if (!procelem->value)
		return ret_errno(ENOMEM);

	proclist->elem = move_ptr(procelem);
	lxc_list_add_tail(&lxc_conf->procs, move_ptr(proclist));

	return 0;
}

static int set_config_idmaps(const char *key, const char *value,
			     struct lxc_conf *lxc_conf, void *data)
{
	__do_free struct lxc_list *idmaplist = NULL;
	__do_free struct id_map *idmap = NULL;
	unsigned long hostid, nsid, range;
	char type;
	int ret;

	if (lxc_config_value_empty(value))
		return lxc_clear_idmaps(lxc_conf);

	idmaplist = malloc(sizeof(*idmaplist));
	if (!idmaplist)
		return ret_errno(ENOMEM);

	idmap = malloc(sizeof(*idmap));
	if (!idmap)
		return ret_errno(ENOMEM);
	memset(idmap, 0, sizeof(*idmap));

	ret = parse_idmaps(value, &type, &nsid, &hostid, &range);
	if (ret < 0)
		return log_error_errno(-EINVAL, EINVAL, "Failed to parse id mappings");

	INFO("Read uid map: type %c nsid %lu hostid %lu range %lu", type, nsid, hostid, range);
	if (type == 'u')
		idmap->idtype = ID_TYPE_UID;
	else if (type == 'g')
		idmap->idtype = ID_TYPE_GID;
	else
		return ret_errno(EINVAL);

	idmap->hostid = hostid;
	idmap->nsid = nsid;
	idmap->range = range;
	idmaplist->elem = idmap;
	lxc_list_add_tail(&lxc_conf->id_map, idmaplist);

	if (!lxc_conf->root_nsuid_map && idmap->idtype == ID_TYPE_UID)
		if (idmap->nsid == 0)
			lxc_conf->root_nsuid_map = idmap;

	if (!lxc_conf->root_nsgid_map && idmap->idtype == ID_TYPE_GID)
		if (idmap->nsid == 0)
			lxc_conf->root_nsgid_map = idmap;

	move_ptr(idmap);
	move_ptr(idmaplist);

	return 0;
}

static int set_config_mount_fstab(const char *key, const char *value,
				  struct lxc_conf *lxc_conf, void *data)
{
	if (lxc_config_value_empty(value)) {
		clr_config_mount_fstab(key, lxc_conf, NULL);
		return ret_errno(EINVAL);
	}

	return set_config_path_item(&lxc_conf->fstab, value);
}

static int set_config_mount_auto(const char *key, const char *value,
				 struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *autos = NULL;
	char *token;
	int i;
	static struct {
		const char *token;
		int mask;
		int flag;
	} allowed_auto_mounts[] = {
		{ "proc",			LXC_AUTO_PROC_MASK,	LXC_AUTO_PROC_MIXED					},
		{ "proc:mixed",			LXC_AUTO_PROC_MASK,	LXC_AUTO_PROC_MIXED					},
		{ "proc:rw",			LXC_AUTO_PROC_MASK,	LXC_AUTO_PROC_RW					},
		{ "sys",			LXC_AUTO_SYS_MASK,	LXC_AUTO_SYS_MIXED					},
		{ "sys:ro",			LXC_AUTO_SYS_MASK,	LXC_AUTO_SYS_RO						},
		{ "sys:mixed",			LXC_AUTO_SYS_MASK,	LXC_AUTO_SYS_MIXED					},
		{ "sys:rw",			LXC_AUTO_SYS_MASK,	LXC_AUTO_SYS_RW						},
		{ "cgroup",			LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_NOSPEC					},
		{ "cgroup:mixed",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_MIXED					},
		{ "cgroup:ro",			LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_RO					},
		{ "cgroup:rw",			LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_RW					},
		{ "cgroup:force",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_NOSPEC | LXC_AUTO_CGROUP_FORCE		},
		{ "cgroup:mixed:force",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_MIXED | LXC_AUTO_CGROUP_FORCE		},
		{ "cgroup:ro:force",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_RO | LXC_AUTO_CGROUP_FORCE		},
		{ "cgroup:rw:force",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_RW | LXC_AUTO_CGROUP_FORCE		},
		{ "cgroup-full",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_NOSPEC				},
		{ "cgroup-full:mixed",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_MIXED				},
		{ "cgroup-full:ro",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_RO					},
		{ "cgroup-full:rw",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_RW					},
		{ "cgroup-full:force",		LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_NOSPEC | LXC_AUTO_CGROUP_FORCE	},
		{ "cgroup-full:mixed:force",	LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_MIXED | LXC_AUTO_CGROUP_FORCE	},
		{ "cgroup-full:ro:force",	LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_RO | LXC_AUTO_CGROUP_FORCE		},
		{ "cgroup-full:rw:force",	LXC_AUTO_CGROUP_MASK,	LXC_AUTO_CGROUP_FULL_RW | LXC_AUTO_CGROUP_FORCE		},
		{ "shmounts:",			LXC_AUTO_SHMOUNTS_MASK,	LXC_AUTO_SHMOUNTS					},
		/*
		 * For adding anything that is just a single on/off, but has no
		 * options: keep mask and flag identical and just define the
		 * enum value as an unused bit so far
		 */
		{ NULL,				0,			0							}
	};

	if (lxc_config_value_empty(value)) {
		lxc_conf->auto_mounts = 0;
		return 0;
	}

	autos = strdup(value);
	if (!autos)
		return ret_errno(ENOMEM);

	lxc_iterate_parts(token, autos, " \t") {
		bool is_shmounts = false;

		for (i = 0; allowed_auto_mounts[i].token; i++) {
			if (!strcmp(allowed_auto_mounts[i].token, token))
				break;

			if (strcmp("shmounts:", allowed_auto_mounts[i].token) == 0 &&
			    strncmp("shmounts:", token, STRLITERALLEN("shmounts:")) == 0) {
				is_shmounts = true;
				break;
			}
		}

		if (!allowed_auto_mounts[i].token)
			return log_error_errno(-EINVAL, EINVAL, "Invalid filesystem to automount \"%s\"", token);

		lxc_conf->auto_mounts &= ~allowed_auto_mounts[i].mask;
		lxc_conf->auto_mounts |= allowed_auto_mounts[i].flag;

		if (is_shmounts) {
			__do_free char *container_path = NULL, *host_path = NULL;
			char *val;

			val = token + STRLITERALLEN("shmounts:");
			if (*val == '\0')
				return log_error_errno(-EINVAL, EINVAL, "Failed to copy shmounts host path");

			host_path = strdup(val);
			if (!host_path)
				return log_error_errno(-EINVAL, EINVAL, "Failed to copy shmounts host path");

			val = strchr(host_path, ':');
			if (!val || *(val + 1) == '\0')
				val = "/dev/.lxc-mounts";
			else
				*val++ = '\0';

			container_path = strdup(val);
			if(!container_path)
				return log_error_errno(-EINVAL, EINVAL, "Failed to copy shmounts container path");

			lxc_conf->shmount.path_host = move_ptr(host_path);
			lxc_conf->shmount.path_cont = move_ptr(container_path);
		}
	}

	return 0;
}

static int set_config_mount(const char *key, const char *value,
			    struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *mntelem = NULL;
	__do_free struct lxc_list *mntlist = NULL;

	if (lxc_config_value_empty(value))
		return lxc_clear_mount_entries(lxc_conf);

	mntlist = malloc(sizeof(*mntlist));
	if (!mntlist)
		return ret_errno(ENOMEM);

	mntelem = strdup(value);
	if (!mntelem)
		return ret_errno(ENOMEM);

	mntlist->elem = move_ptr(mntelem);
	lxc_list_add_tail(&lxc_conf->mount_list, move_ptr(mntlist));

	return 0;
}

int add_elem_to_mount_list(const char *value, struct lxc_conf *lxc_conf) {
	return set_config_mount(NULL, value, lxc_conf, NULL);
}

static int set_config_cap_keep(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *keepcaps = NULL;
	__do_free struct lxc_list *keeplist = NULL;
	char *token;

	if (lxc_config_value_empty(value))
		return lxc_clear_config_keepcaps(lxc_conf);

	keepcaps = strdup(value);
	if (!keepcaps)
		return ret_errno(ENOMEM);

	/* In case several capability keep is specified in a single line
	 * split these caps in a single element for the list.
	 */
	lxc_iterate_parts(token, keepcaps, " \t") {
		if (!strcmp(token, "none"))
			lxc_clear_config_keepcaps(lxc_conf);

		keeplist = malloc(sizeof(*keeplist));
		if (!keeplist)
			return ret_errno(ENOMEM);

		keeplist->elem = strdup(token);
		if (!keeplist->elem)
			return ret_errno(ENOMEM);

		lxc_list_add_tail(&lxc_conf->keepcaps, move_ptr(keeplist));
	}

	return 0;
}

static int set_config_cap_drop(const char *key, const char *value,
			       struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *dropcaps = NULL;
	__do_free struct lxc_list *droplist = NULL;
	char *token;

	if (lxc_config_value_empty(value))
		return lxc_clear_config_caps(lxc_conf);

	dropcaps = strdup(value);
	if (!dropcaps)
		return ret_errno(ENOMEM);

	/* In case several capability drop is specified in a single line
	 * split these caps in a single element for the list.
	 */
	lxc_iterate_parts(token, dropcaps, " \t") {
		droplist = malloc(sizeof(*droplist));
		if (!droplist)
			return ret_errno(ENOMEM);

		droplist->elem = strdup(token);
		if (!droplist->elem)
			return ret_errno(ENOMEM);

		lxc_list_add_tail(&lxc_conf->caps, move_ptr(droplist));
	}

	return 0;
}

static int set_config_console_path(const char *key, const char *value,
				   struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->console.path, value);
}

static int set_config_console_rotate(const char *key, const char *value,
				     struct lxc_conf *lxc_conf, void *data)
{
	int ret;

	if (lxc_config_value_empty(value)) {
		lxc_conf->console.log_rotate = 0;
		return 0;
	}

	ret = lxc_safe_uint(value, &lxc_conf->console.log_rotate);
	if (ret)
		return ret_errno(EINVAL);

	if (lxc_conf->console.log_rotate > 1)
		return log_error_errno(-EINVAL, EINVAL, "The \"lxc.console.rotate\" config key can only be set to 0 or 1");

	return 0;
}

static int set_config_console_logfile(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->console.log_path, value);
}

static int set_config_console_buffer_size(const char *key, const char *value,
					  struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	int64_t size;
	uint64_t buffer_size, pgsz;

	if (lxc_config_value_empty(value)) {
		lxc_conf->console.buffer_size = 0;
		return 0;
	}

	/* If the user specified "auto" the default log size is 2^17 = 128 Kib */
	if (!strcmp(value, "auto")) {
		lxc_conf->console.buffer_size = 1 << 17;
		return 0;
	}

	ret = parse_byte_size_string(value, &size);
	if (ret)
		return ret;

	if (size < 0)
		return ret_errno(EINVAL);

	/* must be at least a page size */
	pgsz = lxc_getpagesize();
	if ((uint64_t)size < pgsz) {
		NOTICE("Requested ringbuffer size for the console is %" PRId64 " but must be at least %" PRId64 " bytes. Setting ringbuffer size to %" PRId64 " bytes",
		       size, pgsz, pgsz);
		size = pgsz;
	}

	buffer_size = lxc_find_next_power2((uint64_t)size);
	if (buffer_size == 0)
		return ret_errno(EINVAL);

	if (buffer_size != size)
		NOTICE("Passed size was not a power of 2. Rounding log size to next power of two: %" PRIu64 " bytes", buffer_size);

	lxc_conf->console.buffer_size = buffer_size;

	return 0;
}

static int set_config_console_size(const char *key, const char *value,
				   struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	int64_t size;
	uint64_t log_size, pgsz;

	if (lxc_config_value_empty(value)) {
		lxc_conf->console.log_size = 0;
		return 0;
	}

	/* If the user specified "auto" the default log size is 2^17 = 128 Kib */
	if (!strcmp(value, "auto")) {
		lxc_conf->console.log_size = 1 << 17;
		return 0;
	}

	ret = parse_byte_size_string(value, &size);
	if (ret)
		return ret_errno(EINVAL);

	if (size < 0)
		return ret_errno(EINVAL);

	/* must be at least a page size */
	pgsz = lxc_getpagesize();
	if ((uint64_t)size < pgsz) {
		NOTICE("Requested ringbuffer size for the console is %" PRId64 " but must be at least %" PRId64 " bytes. Setting ringbuffer size to %" PRId64 " bytes",
		       size, pgsz, pgsz);
		size = pgsz;
	}

	log_size = lxc_find_next_power2((uint64_t)size);
	if (log_size == 0)
		return ret_errno(EINVAL);

	if (log_size != size)
		NOTICE("Passed size was not a power of 2. Rounding log size to next power of two: %" PRIu64 " bytes", log_size);

	lxc_conf->console.log_size = log_size;

	return 0;
}

/*
 * If we find a lxc.net.[i].hwaddr or lxc.network.hwaddr in the original config
 * file, we expand it in the unexpanded_config, so that after a save_config we
 * store the hwaddr for re-use.
 * This is only called when reading the config file, not when executing a
 * lxc.include.
 * 'x' and 'X' are substituted in-place.
 */
static void update_hwaddr(const char *line)
{
	char *p;

	line += lxc_char_left_gc(line, strlen(line));
	if (line[0] == '#')
		return;

	if (!lxc_config_net_is_hwaddr(line))
		return;

	/* Let config_net_hwaddr raise the error. */
	p = strchr(line, '=');
	if (!p)
		return;
	p++;

	while (isblank(*p))
		p++;

	if (!*p)
		return;

	rand_complete_hwaddr(p);
}

int append_unexp_config_line(const char *line, struct lxc_conf *conf)
{
	size_t linelen;
	size_t len = conf->unexpanded_len;

	update_hwaddr(line);

	linelen = strlen(line);
	while (conf->unexpanded_alloced <= len + linelen + 2) {
		char *tmp;

		tmp = realloc(conf->unexpanded_config, conf->unexpanded_alloced + 1024);
		if (!tmp)
			return ret_errno(EINVAL);

		if (!conf->unexpanded_config)
			*tmp = '\0';

		conf->unexpanded_config = tmp;
		conf->unexpanded_alloced += 1024;
	}

	memcpy(conf->unexpanded_config + conf->unexpanded_len, line, linelen);
	conf->unexpanded_len += linelen;
	if (line[linelen - 1] != '\n')
		conf->unexpanded_config[conf->unexpanded_len++] = '\n';
	conf->unexpanded_config[conf->unexpanded_len] = '\0';

	return 0;
}

static int do_includedir(const char *dirp, struct lxc_conf *lxc_conf)
{
	__do_closedir DIR *dir = NULL;
	struct dirent *direntp;
	int len, ret;

	dir = opendir(dirp);
	if (!dir)
		return -errno;

	while ((direntp = readdir(dir))) {
		const char *fnam;
		char path[PATH_MAX];

		fnam = direntp->d_name;
		if (!strcmp(fnam, "."))
			continue;

		if (!strcmp(fnam, ".."))
			continue;

		len = strlen(fnam);
		if (len < 6 || strncmp(fnam + len - 5, ".conf", 5) != 0)
			continue;

		len = snprintf(path, PATH_MAX, "%s/%s", dirp, fnam);
		if (len < 0 || len >= PATH_MAX)
			return ret_errno(EIO);

		ret = lxc_config_read(path, lxc_conf, true);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int set_config_includefiles(const char *key, const char *value,
				   struct lxc_conf *lxc_conf, void *data)
{
	if (lxc_config_value_empty(value)) {
		clr_config_includefiles(key, lxc_conf, NULL);
		return 0;
	}

	if (is_dir(value))
		return do_includedir(value, lxc_conf);

	return lxc_config_read(value, lxc_conf, true);
}

static int set_config_rootfs_path(const char *key, const char *value,
				  struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *dup = NULL;
	int ret;
	char *tmp;
	const char *container_path;

	if (lxc_config_value_empty(value)) {
		free(lxc_conf->rootfs.path);
		lxc_conf->rootfs.path = NULL;
		return 0;
	}

	dup = strdup(value);
	if (!dup)
		return ret_errno(ENOMEM);

	/* Split <storage type>:<container path> into <storage type> and
	 * <container path>. Set "rootfs.bdev_type" to <storage type> and
	 * "rootfs.path" to <container path>.
	 */
	tmp = strchr(dup, ':');
	if (tmp) {
		*tmp = '\0';

		ret = set_config_path_item(&lxc_conf->rootfs.bdev_type, dup);
		if (ret < 0)
			return ret_errno(ENOMEM);

		tmp++;
		container_path = tmp;
	} else {
		container_path = value;
	}

	return set_config_path_item(&lxc_conf->rootfs.path, container_path);
}

static int set_config_rootfs_managed(const char *key, const char *value,
				     struct lxc_conf *lxc_conf, void *data)
{
	return set_config_bool_item(&lxc_conf->rootfs.managed, value, true);
}

static int set_config_rootfs_mount(const char *key, const char *value,
				   struct lxc_conf *lxc_conf, void *data)
{
	return set_config_path_item(&lxc_conf->rootfs.mount, value);
}

static int set_config_rootfs_options(const char *key, const char *value,
				     struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *mdata = NULL, *opts = NULL;
	unsigned long mflags = 0, pflags = 0;
	struct lxc_rootfs *rootfs = &lxc_conf->rootfs;
	int ret;

	ret = parse_mntopts(value, &mflags, &mdata);
	if (ret < 0)
		return ret_errno(EINVAL);

	ret = parse_propagationopts(value, &pflags);
	if (ret < 0)
		return ret_errno(EINVAL);

	ret = set_config_string_item(&opts, value);
	if (ret < 0)
		return ret_errno(ENOMEM);

	rootfs->mountflags = mflags | pflags;
	rootfs->options = move_ptr(opts);
	rootfs->data = move_ptr(mdata);

	return 0;
}

static int set_config_uts_name(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	__do_free struct utsname *utsname = NULL;

	if (lxc_config_value_empty(value)) {
		clr_config_uts_name(key, lxc_conf, NULL);
		return 0;
	}

	utsname = malloc(sizeof(*utsname));
	if (!utsname)
		return ret_errno(ENOMEM);

	if (strlen(value) >= sizeof(utsname->nodename))
		return ret_errno(EINVAL);

	(void)strlcpy(utsname->nodename, value, sizeof(utsname->nodename));
	free(lxc_conf->utsname);
	lxc_conf->utsname = move_ptr(utsname);

	return 0;
}

static int set_config_namespace_clone(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *ns = NULL;
	char *token;
	int cloneflag = 0;

	if (lxc_config_value_empty(value))
		return clr_config_namespace_clone(key, lxc_conf, data);

	if (lxc_conf->ns_keep != 0)
		return log_error_errno(-EINVAL, EINVAL, "Cannot set both \"lxc.namespace.clone\" and \"lxc.namespace.keep\"");

	ns = strdup(value);
	if (!ns)
		return ret_errno(ENOMEM);

	lxc_iterate_parts(token, ns, " \t") {
		token += lxc_char_left_gc(token, strlen(token));
		token[lxc_char_right_gc(token, strlen(token))] = '\0';
		cloneflag = lxc_namespace_2_cloneflag(token);
		if (cloneflag < 0)
			return ret_errno(EINVAL);
		lxc_conf->ns_clone |= cloneflag;
	}

	return 0;
}

static int set_config_namespace_keep(const char *key, const char *value,
				     struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *ns = NULL;
	char *token;
	int cloneflag = 0;

	if (lxc_config_value_empty(value))
		return clr_config_namespace_keep(key, lxc_conf, data);

	if (lxc_conf->ns_clone != 0)
		return log_error_errno(-EINVAL, EINVAL, "Cannot set both \"lxc.namespace.clone\" and \"lxc.namespace.keep\"");

	ns = strdup(value);
	if (!ns)
		return ret_errno(ENOMEM);

	lxc_iterate_parts(token, ns, " \t") {
		token += lxc_char_left_gc(token, strlen(token));
		token[lxc_char_right_gc(token, strlen(token))] = '\0';
		cloneflag = lxc_namespace_2_cloneflag(token);
		if (cloneflag < 0)
			return ret_errno(EINVAL);
		lxc_conf->ns_keep |= cloneflag;
	}

	return 0;
}

static int set_config_namespace_share(const char *key, const char *value,
				      struct lxc_conf *lxc_conf, void *data)
{
	int ns_idx;
	const char *namespace;

	if (lxc_config_value_empty(value))
		return clr_config_namespace_share(key, lxc_conf, data);

	namespace = key + STRLITERALLEN("lxc.namespace.share.");
	ns_idx = lxc_namespace_2_ns_idx(namespace);
	if (ns_idx < 0)
		return ns_idx;

	return set_config_string_item(&lxc_conf->ns_share[ns_idx], value);
}

struct parse_line_conf {
	struct lxc_conf *conf;
	bool from_include;
};

static int parse_line(char *buffer, void *data)
{
	__do_free char *linep = NULL;
	char *dot, *key, *line, *value;
	bool empty_line;
	struct lxc_config_t *config;
	int ret;
	char *dup = buffer;
	struct parse_line_conf *plc = data;

	/* If there are newlines in the config file we should keep them. */
	empty_line = lxc_is_line_empty(dup);
	if (empty_line)
		dup = "\n";

	/* We have to dup the buffer otherwise, at the re-exec for reboot we
	 * modified the original string on the stack by replacing '=' by '\0'
	 * below.
	 */
	linep = line = strdup(dup);
	if (!line)
		return ret_errno(ENOMEM);

	if (!plc->from_include) {
		ret = append_unexp_config_line(line, plc->conf);
		if (ret < 0)
			return ret;
	}

	if (empty_line)
		return 0;

	line += lxc_char_left_gc(line, strlen(line));

	/* ignore comments */
	if (line[0] == '#')
		return 0;

	/* martian option - don't add it to the config itself */
	if (strncmp(line, "lxc.", 4))
		return 0;

	dot = strchr(line, '=');
	if (!dot)
		return log_error_errno(-EINVAL, EINVAL, "Invalid configuration line: %s", line);

	*dot = '\0';
	value = dot + 1;

	key = line;
	key[lxc_char_right_gc(key, strlen(key))] = '\0';

	value += lxc_char_left_gc(value, strlen(value));
	value[lxc_char_right_gc(value, strlen(value))] = '\0';

	if (*value == '\'' || *value == '\"') {
		size_t len;

		len = strlen(value);
		if (len > 1 && value[len - 1] == *value) {
			value[len - 1] = '\0';
			value++;
		}
	}

	config = lxc_get_config(key);
	if (!config)
		return log_error_errno(-EINVAL, EINVAL, "Unknown configuration key \"%s\"", key);

	return config->set(key, value, plc->conf, NULL);
}

static struct new_config_item *parse_new_conf_line(char *buffer)
{
	__do_free char *k = NULL, *linep = NULL, *v = NULL;
	__do_free struct new_config_item *new = NULL;
	char *dup = buffer;
	char *dot, *key, *line, *value;

	linep = line = strdup(dup);
	if (!line)
		return NULL;

	line += lxc_char_left_gc(line, strlen(line));

	/* martian option - don't add it to the config itself */
	if (strncmp(line, "lxc.", 4))
		return 0;

	dot = strchr(line, '=');
	if (!dot)
		return log_error_errno(NULL, EINVAL, "Invalid configuration line: %s", line);

	*dot = '\0';
	value = dot + 1;

	key = line;
	key[lxc_char_right_gc(key, strlen(key))] = '\0';

	value += lxc_char_left_gc(value, strlen(value));
	value[lxc_char_right_gc(value, strlen(value))] = '\0';

	if (*value == '\'' || *value == '\"') {
		size_t len;

		len = strlen(value);
		if (len > 1 && value[len - 1] == *value) {
			value[len - 1] = '\0';
			value++;
		}
	}

	new = malloc(sizeof(struct new_config_item));
	if (!new)
		return NULL;

	k = strdup(key);
	if (!k)
		return NULL;

	v = strdup(value);
	if (!v)
		return NULL;

	new->key = move_ptr(k);
	new->val = move_ptr(v);
	return move_ptr(new);
}

int lxc_config_read(const char *file, struct lxc_conf *conf, bool from_include)
{
	struct parse_line_conf c;

	c.conf = conf;
	c.from_include = from_include;

	/* Catch only the top level config file name in the structure. */
	if (!conf->rcfile)
		conf->rcfile = strdup(file);

	return lxc_file_for_each_line_mmap(file, parse_line, &c);
}

int lxc_config_define_add(struct lxc_list *defines, char *arg)
{
	__do_free struct lxc_list *dent = NULL;

	dent = malloc(sizeof(struct lxc_list));
	if (!dent)
		return ret_errno(ENOMEM);

	dent->elem = parse_new_conf_line(arg);
	if (!dent->elem)
		return ret_errno(ENOMEM);

	lxc_list_add_tail(defines, move_ptr(dent));

	return 0;
}

bool lxc_config_define_load(struct lxc_list *defines, struct lxc_container *c)
{
	struct lxc_list *it;
	bool bret = true;

	lxc_list_for_each(it, defines) {
		struct new_config_item *new_item = it->elem;
		bret = c->set_config_item(c, new_item->key, new_item->val);
		if (!bret)
			break;
	}

	lxc_config_define_free(defines);

	return bret;
}

void lxc_config_define_free(struct lxc_list *defines)
{
	struct lxc_list *it, *next;

	lxc_list_for_each_safe(it, defines, next) {
		struct new_config_item *new_item = it->elem;
		free(new_item->key);
		free(new_item->val);
		lxc_list_del(it);
		free(it);
	}
}

signed long lxc_config_parse_arch(const char *arch)
{
#if HAVE_SYS_PERSONALITY_H
	struct per_name {
		char *name;
		unsigned long per;
	} pername[] = {
		{ "arm",       PER_LINUX32 },
		{ "armel",     PER_LINUX32 },
		{ "armhf",     PER_LINUX32 },
		{ "armv7l",    PER_LINUX32 },
		{ "athlon",    PER_LINUX32 },
		{ "i386",      PER_LINUX32 },
		{ "i486",      PER_LINUX32 },
		{ "i586",      PER_LINUX32 },
		{ "i686",      PER_LINUX32 },
		{ "linux32",   PER_LINUX32 },
		{ "mips",      PER_LINUX32 },
		{ "mipsel",    PER_LINUX32 },
		{ "ppc",       PER_LINUX32 },
		{ "powerpc",   PER_LINUX32 },
		{ "x86",       PER_LINUX32 },
		{ "amd64",     PER_LINUX   },
		{ "arm64",     PER_LINUX   },
		{ "linux64",   PER_LINUX   },
		{ "mips64",    PER_LINUX   },
		{ "mips64el",  PER_LINUX   },
		{ "ppc64",     PER_LINUX   },
		{ "ppc64el",   PER_LINUX   },
		{ "ppc64le",   PER_LINUX   },
		{ "powerpc64", PER_LINUX   },
		{ "s390x",     PER_LINUX   },
		{ "x86_64",    PER_LINUX   },
	};
	size_t len = sizeof(pername) / sizeof(pername[0]);

	for (int i = 0; i < len; i++)
		if (!strcmp(pername[i].name, arch))
			return pername[i].per;
#endif

	return -1;
}

int lxc_fill_elevated_privileges(char *flaglist, int *flags)
{
	char *token;
	int i, aflag;
	struct {
		const char *token;
		int flag;
	} all_privs[] = {
		{ "CGROUP", LXC_ATTACH_MOVE_TO_CGROUP    },
		{ "CAP",    LXC_ATTACH_DROP_CAPABILITIES },
		{ "LSM",    LXC_ATTACH_LSM_EXEC          },
		{ NULL,     0                            }
	};

	if (!flaglist) {
		/* For the sake of backward compatibility, drop all privileges
		*  if none is specified.
		 */
		for (i = 0; all_privs[i].token; i++)
			*flags |= all_privs[i].flag;

		return 0;
	}

	lxc_iterate_parts(token, flaglist, "|") {
		aflag = -1;

		for (i = 0; all_privs[i].token; i++)
			if (!strcmp(all_privs[i].token, token))
				aflag = all_privs[i].flag;

		if (aflag < 0)
			return ret_errno(EINVAL);

		*flags |= aflag;
	}

	return 0;
}

/* Write out a configuration file. */
int write_config(int fd, const struct lxc_conf *conf)
{
	int ret;
	size_t len = conf->unexpanded_len;

	if (len == 0)
		return 0;

	ret = lxc_write_nointr(fd, conf->unexpanded_config, len);
	if (ret < 0)
		return log_error_errno(-errno, errno, "Failed to write configuration file");

	return 0;
}

bool do_append_unexp_config_line(struct lxc_conf *conf, const char *key,
				 const char *v)
{
	__do_free char *tmp = NULL;
	int ret;
	size_t len;

	len = strlen(key) + strlen(v) + 4;
	tmp = must_realloc(NULL, len);

	if (lxc_config_value_empty(v))
		ret = snprintf(tmp, len, "%s =", key);
	else
		ret = snprintf(tmp, len, "%s = %s", key, v);
	if (ret < 0 || ret >= len)
		return false;

	/* Save the line verbatim into unexpanded_conf */
	if (append_unexp_config_line(tmp, conf))
		return false;

	return true;
}

void clear_unexp_config_line(struct lxc_conf *conf, const char *key,
			     bool rm_subkeys)
{
	char *lend;
	char *lstart = conf->unexpanded_config;

	if (!conf->unexpanded_config)
		return;

	while (*lstart) {
		lend = strchr(lstart, '\n');
		char v;

		if (!lend)
			lend = lstart + strlen(lstart);
		else
			lend++;

		if (strncmp(lstart, key, strlen(key)) != 0) {
			lstart = lend;
			continue;
		}

		if (!rm_subkeys) {
			v = lstart[strlen(key)];
			if (!isspace(v) && v != '=') {
				lstart = lend;
				continue;
			}
		}

		conf->unexpanded_len -= (lend - lstart);

		if (*lend == '\0') {
			*lstart = '\0';
			return;
		}

		memmove(lstart, lend, strlen(lend) + 1);
	}
}

bool clone_update_unexp_ovl_paths(struct lxc_conf *conf, const char *oldpath,
				  const char *newpath, const char *oldname,
				  const char *newname, const char *ovldir)
{
	__do_free char *newdir = NULL, *olddir = NULL;
	char *lstart = conf->unexpanded_config;
	const char *key = "lxc.mount.entry";
	int ret;
	char *lend, *p, *q;
	size_t newdirlen, olddirlen;

	olddirlen = strlen(ovldir) + strlen(oldpath) + strlen(oldname) + 2;
	olddir = must_realloc(NULL, olddirlen + 1);
	ret = snprintf(olddir, olddirlen + 1, "%s=%s/%s", ovldir, oldpath,
		       oldname);
	if (ret < 0 || ret >= olddirlen + 1)
		return false;

	newdirlen = strlen(ovldir) + strlen(newpath) + strlen(newname) + 2;
	newdir = must_realloc(NULL, newdirlen + 1);
	ret = snprintf(newdir, newdirlen + 1, "%s=%s/%s", ovldir, newpath,
		       newname);
	if (ret < 0 || ret >= newdirlen + 1)
		return false;

	if (!conf->unexpanded_config)
		return true;

	while (*lstart) {
		lend = strchr(lstart, '\n');
		if (!lend)
			lend = lstart + strlen(lstart);
		else
			lend++;

		if (strncmp(lstart, key, strlen(key)) != 0)
			goto next;

		p = strchr(lstart + strlen(key), '=');
		if (!p)
			goto next;
		p++;

		while (isblank(*p))
			p++;

		if (p >= lend)
			goto next;

		/* Whenever a lxc.mount.entry entry is found in a line we check
		*  if the substring "overlay" is present before doing any
		*  further work. We check for "overlay" because substrings need
		*  to have at least one space before them in a valid overlay
		*  lxc.mount.entry (/A B overlay).  When the space before is
		*  missing it is very likely that these substrings are part of a
		*  path or something else. (Checking q >= lend ensures that we
		*  only count matches in the current line.) */
		q = strstr(p, " overlay");
		if (!q || q >= lend)
			goto next;

		if (!(q = strstr(p, olddir)) || (q >= lend))
			goto next;

		/* replace the olddir with newdir */
		if (olddirlen >= newdirlen) {
			size_t diff = olddirlen - newdirlen;
			memcpy(q, newdir, newdirlen);

			if (olddirlen != newdirlen) {
				memmove(q + newdirlen, q + newdirlen + diff,
				        strlen(q) - newdirlen - diff + 1);
				lend -= diff;
				conf->unexpanded_len -= diff;
			}
		} else {
			char *new;
			size_t diff = newdirlen - olddirlen;
			size_t oldlen = conf->unexpanded_len;
			size_t newlen = oldlen + diff;
			size_t poffset = q - conf->unexpanded_config;

			new = realloc(conf->unexpanded_config, newlen + 1);
			if (!new)
				return false;

			conf->unexpanded_len = newlen;
			conf->unexpanded_alloced = newlen + 1;
			new[newlen - 1] = '\0';
			lend = new + (lend - conf->unexpanded_config);

			/* Move over the remainder to make room for the newdir.
			 */
			memmove(new + poffset + newdirlen,
			        new + poffset + olddirlen,
			        oldlen - poffset - olddirlen + 1);
			conf->unexpanded_config = new;

			memcpy(new + poffset, newdir, newdirlen);
			lend += diff;
		}

	next:
		lstart = lend;
	}

	return true;
}

bool clone_update_unexp_hooks(struct lxc_conf *conf, const char *oldpath,
			      const char *newpath, const char *oldname,
			      const char *newname)
{
	__do_free char *newdir = NULL, *olddir = NULL;
	char *lstart = conf->unexpanded_config;
	const char *key = "lxc.hook";
	int ret;
	char *lend, *p;
	size_t newdirlen, olddirlen;

	olddirlen = strlen(oldpath) + strlen(oldname) + 1;
	olddir = must_realloc(NULL, olddirlen + 1);
	ret = snprintf(olddir, olddirlen + 1, "%s/%s", oldpath, oldname);
	if (ret < 0 || ret >= olddirlen + 1)
		return false;

	newdirlen = strlen(newpath) + strlen(newname) + 1;
	newdir = must_realloc(NULL, newdirlen + 1);
	ret = snprintf(newdir, newdirlen + 1, "%s/%s", newpath, newname);
	if (ret < 0 || ret >= newdirlen + 1)
		return false;

	if (!conf->unexpanded_config)
		return true;

	while (*lstart) {
		lend = strchr(lstart, '\n');
		if (!lend)
			lend = lstart + strlen(lstart);
		else
			lend++;

		if (strncmp(lstart, key, strlen(key)) != 0)
			goto next;

		p = strchr(lstart + strlen(key), '=');
		if (!p)
			goto next;
		p++;

		while (isblank(*p))
			p++;

		if (p >= lend)
			goto next;

		if (strncmp(p, olddir, strlen(olddir)) != 0)
			goto next;

		/* replace the olddir with newdir */
		if (olddirlen >= newdirlen) {
			size_t diff = olddirlen - newdirlen;
			memcpy(p, newdir, newdirlen);

			if (olddirlen != newdirlen) {
				memmove(p + newdirlen, p + newdirlen + diff,
				        strlen(p) - newdirlen - diff + 1);
				lend -= diff;
				conf->unexpanded_len -= diff;
			}
		} else {
			char *new;
			size_t diff = newdirlen - olddirlen;
			size_t oldlen = conf->unexpanded_len;
			size_t newlen = oldlen + diff;
			size_t poffset = p - conf->unexpanded_config;

			new = realloc(conf->unexpanded_config, newlen + 1);
			if (!new)
				return false;

			conf->unexpanded_len = newlen;
			conf->unexpanded_alloced = newlen + 1;
			new[newlen - 1] = '\0';
			lend = new + (lend - conf->unexpanded_config);

			/* Move over the remainder to make room for the newdir.
			 */
			memmove(new + poffset + newdirlen,
			        new + poffset + olddirlen,
			        oldlen - poffset - olddirlen + 1);
			conf->unexpanded_config = new;

			memcpy(new + poffset, newdir, newdirlen);
			lend += diff;
		}

	next:
		lstart = lend;
	}

	return true;
}

#define DO(cmd)                                                                \
	{                                                                      \
		if (!(cmd)) {                                                  \
			ERROR("Error writing to new config");                  \
			return false;                                          \
		}                                                              \
	}

/* This is called only from clone.  We wish to update all hwaddrs in the
 * unexpanded config file. We can't/don't want to update any which come from
 * lxc.includes (there shouldn't be any).
 * We can't just walk the c->lxc-conf->network list because that includes netifs
 * from the include files.  So we update the ones which we find in the unexp
 * config file, then find the original macaddr in the conf->network, and update
 * that to the same value.
 */
bool network_new_hwaddrs(struct lxc_conf *conf)
{
	char *lend, *p, *p2;
	struct lxc_list *it;
	char *lstart = conf->unexpanded_config;

	if (!conf->unexpanded_config)
		return true;

	while (*lstart) {
		char newhwaddr[18], oldhwaddr[17];

		lend = strchr(lstart, '\n');
		if (!lend)
			lend = lstart + strlen(lstart);
		else
			lend++;

		if (!lxc_config_net_is_hwaddr(lstart)) {
			lstart = lend;
			continue;
		}

		p = strchr(lstart, '=');
		if (!p) {
			lstart = lend;
			continue;
		}

		p++;
		while (isblank(*p))
			p++;
		if (!*p)
			return true;

		p2 = p;
		while (*p2 && !isblank(*p2) && *p2 != '\n')
			p2++;

		if ((p2 - p) != 17) {
			WARN("Bad hwaddr entry");
			lstart = lend;
			continue;
		}

		memcpy(oldhwaddr, p, 17);

		if (!new_hwaddr(newhwaddr))
			return false;

		memcpy(p, newhwaddr, 17);
		lxc_list_for_each(it, &conf->network) {
			struct lxc_netdev *n = it->elem;

			if (n->hwaddr && memcmp(oldhwaddr, n->hwaddr, 17) == 0)
				memcpy(n->hwaddr, newhwaddr, 17);
		}

		lstart = lend;
	}

	return true;
}

static int set_config_ephemeral(const char *key, const char *value,
				struct lxc_conf *lxc_conf, void *data)
{
	int ret;

	if (lxc_config_value_empty(value)) {
		lxc_conf->ephemeral = 0;
		return 0;
	}

	ret = lxc_safe_uint(value, &lxc_conf->ephemeral);
	if (ret < 0)
		return ret;

	if (lxc_conf->ephemeral > 1)
		return ret_errno(EINVAL);

	return 0;
}

static int set_config_log_syslog(const char *key, const char *value,
			     struct lxc_conf *lxc_conf, void *data)
{
	int facility;

	if (lxc_conf->syslog)
		free_disarm(lxc_conf->syslog);

	if (lxc_config_value_empty(value))
		return 0;

	facility = lxc_syslog_priority_to_int(value);
	if (facility == -EINVAL)
		return ret_errno(EINVAL);

	lxc_log_syslog(facility);

	return set_config_string_item(&lxc_conf->syslog, value);
}

static int set_config_no_new_privs(const char *key, const char *value,
				   struct lxc_conf *lxc_conf, void *data)
{
	int ret;
	unsigned int v;

	if (lxc_config_value_empty(value)) {
		lxc_conf->no_new_privs = false;
		return 0;
	}

	ret = lxc_safe_uint(value, &v);
	if (ret < 0)
		return ret;

	if (v > 1)
		return ret_errno(EINVAL);

	lxc_conf->no_new_privs = v ? true : false;

	return 0;
}

/* Callbacks to get configuration items. */
static int get_config_personality(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

#if HAVE_SYS_PERSONALITY_H
	int len = 0;

	switch (c->personality) {
	case PER_LINUX32:
		strprint(retv, inlen, "i686");
		break;
	case PER_LINUX:
		strprint(retv, inlen, "x86_64");
		break;
	default:
		break;
	}
#endif

	return fulllen;
}

static int get_config_pty_max(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_size_t(c, retv, inlen, c->pty_max);
}

static int get_config_tty_max(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_size_t(c, retv, inlen, c->ttys.max);
}

static int get_config_tty_dir(const char *key, char *retv, int inlen,
			     struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->ttys.dir);
}

static int get_config_apparmor_profile(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->lsm_aa_profile);
}

static int get_config_apparmor_allow_incomplete(const char *key, char *retv,
						int inlen, struct lxc_conf *c,
						void *data)
{
	return lxc_get_conf_int(c, retv, inlen,
				c->lsm_aa_allow_incomplete);
}

static int get_config_apparmor_allow_nesting(const char *key, char *retv,
					     int inlen, struct lxc_conf *c,
					     void *data)
{
	return lxc_get_conf_int(c, retv, inlen,
				c->lsm_aa_allow_nesting);
}

static int get_config_apparmor_raw(const char *key, char *retv,
				   int inlen, struct lxc_conf *c,
				   void *data)
{
	int len;
	struct lxc_list *it;
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->lsm_aa_raw) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_selinux_context(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->lsm_se_context);
}

static int get_config_selinux_context_keyring(const char *key, char *retv, int inlen,
					      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->lsm_se_keyring_context);
}

static int get_config_keyring_session(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_bool(c, retv, inlen, c->keyring_disable_session);
}


/* If you ask for a specific cgroup value, i.e. lxc.cgroup.devices.list, then
 * just the value(s) will be printed. Since there still could be more than one,
 * it is newline-separated.
 * (Maybe that's ambiguous, since some values, i.e. devices.list, will already
 * have newlines?)
 * If you ask for 'lxc.cgroup", then all cgroup entries will be printed, in
 * 'lxc.cgroup.subsystem.key = value' format.
 */
static int __get_config_cgroup_controller(const char *key, char *retv,
					  int inlen, struct lxc_conf *c,
					  int version)
{
	int len;
	size_t namespaced_token_len;
	char *global_token, *namespaced_token;
	struct lxc_list *it;
	int fulllen = 0;
	bool get_all = false;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (version == CGROUP2_SUPER_MAGIC) {
		global_token = "lxc.cgroup2";
		namespaced_token = "lxc.cgroup2.";
		namespaced_token_len = STRLITERALLEN("lxc.cgroup2.");
	} else if (version == CGROUP_SUPER_MAGIC) {
		global_token = "lxc.cgroup";
		namespaced_token = "lxc.cgroup.";
		namespaced_token_len = STRLITERALLEN("lxc.cgroup.");
	} else {
		return ret_errno(EINVAL);
	}

	if (strcmp(key, global_token) == 0)
		get_all = true;
	else if (strncmp(key, namespaced_token, namespaced_token_len) == 0)
		key += namespaced_token_len;
	else
		return ret_errno(EINVAL);

	lxc_list_for_each(it, &c->cgroup) {
		struct lxc_cgroup *cg = it->elem;

		if (get_all) {
			if (version != cg->version)
				continue;

			strprint(retv, inlen, "%s.%s = %s\n", global_token,
				 cg->subsystem, cg->value);
		} else if (strcmp(cg->subsystem, key) == 0) {
			strprint(retv, inlen, "%s\n", cg->value);
		}
	}

	return fulllen;
}

static int get_config_cgroup_controller(const char *key, char *retv, int inlen,
					struct lxc_conf *c, void *data)
{
	return __get_config_cgroup_controller(key, retv, inlen, c,
					      CGROUP_SUPER_MAGIC);
}

static int get_config_cgroup2_controller(const char *key, char *retv, int inlen,
					 struct lxc_conf *c, void *data)
{
	return __get_config_cgroup_controller(key, retv, inlen, c,
					      CGROUP2_SUPER_MAGIC);
}

static int get_config_cgroup_dir(const char *key, char *retv, int inlen,
				 struct lxc_conf *lxc_conf, void *data)
{
	int len;
	int fulllen = 0;

	if (strcmp(key, "lxc.cgroup.dir") != 0)
		return ret_errno(EINVAL);

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	strprint(retv, inlen, "%s", lxc_conf->cgroup_meta.dir);

	return fulllen;
}

static inline int get_config_cgroup_relative(const char *key, char *retv,
					     int inlen, struct lxc_conf *lxc_conf,
					     void *data)
{
	return lxc_get_conf_int(lxc_conf, retv, inlen,
				lxc_conf->cgroup_meta.relative);
}

static int get_config_idmaps(const char *key, char *retv, int inlen,
			     struct lxc_conf *c, void *data)
{
	struct lxc_list *it;
	int len, listlen, ret;
	int fulllen = 0;
/* "u 1000 1000000 65536"
 *
 * let's render this as
 *
 * sizeof(char)
 * +
 * sizeof(" ")
 * +
 * sizeof(uint32_t)
 * +
 * sizeof(" ")
 * +
 * sizeof(uint32_t)
 * +
 * sizeof(" ")
 * +
 * sizeof(uint32_t)
 * +
 * \0
 */
#define __LXC_IDMAP_STR_BUF (3 * INTTYPE_TO_STRLEN(uint32_t) + 3 + 1 + 1)
	char buf[__LXC_IDMAP_STR_BUF];

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	listlen = lxc_list_len(&c->id_map);
	lxc_list_for_each(it, &c->id_map) {
		struct id_map *map = it->elem;
		ret = snprintf(buf, __LXC_IDMAP_STR_BUF, "%c %lu %lu %lu",
			       (map->idtype == ID_TYPE_UID) ? 'u' : 'g',
			       map->nsid, map->hostid, map->range);
		if (ret < 0 || ret >= __LXC_IDMAP_STR_BUF)
			return ret_errno(EIO);

		strprint(retv, inlen, "%s%s", buf, (listlen-- > 1) ? "\n" : "");
	}

	return fulllen;
}

static int get_config_log_level(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	const char *v;
	v = lxc_log_priority_to_string(c->loglevel);
	return lxc_get_conf_str(retv, inlen, v);
}

static int get_config_log_file(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->logfile);
}

static int get_config_mount_fstab(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->fstab);
}

static int get_config_mount_auto(const char *key, char *retv, int inlen,
				 struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	const char *sep = "";

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!(c->auto_mounts & LXC_AUTO_ALL_MASK))
		return 0;

	switch (c->auto_mounts & LXC_AUTO_PROC_MASK) {
	case LXC_AUTO_PROC_MIXED:
		strprint(retv, inlen, "%sproc:mixed", sep);
		sep = " ";
		break;
	case LXC_AUTO_PROC_RW:
		strprint(retv, inlen, "%sproc:rw", sep);
		sep = " ";
		break;
	default:
		break;
	}

	switch (c->auto_mounts & LXC_AUTO_SYS_MASK) {
	case LXC_AUTO_SYS_RO:
		strprint(retv, inlen, "%ssys:ro", sep);
		sep = " ";
		break;
	case LXC_AUTO_SYS_RW:
		strprint(retv, inlen, "%ssys:rw", sep);
		sep = " ";
		break;
	case LXC_AUTO_SYS_MIXED:
		strprint(retv, inlen, "%ssys:mixed", sep);
		sep = " ";
		break;
	default:
		break;
	}

	switch (c->auto_mounts & LXC_AUTO_CGROUP_MASK) {
	case LXC_AUTO_CGROUP_NOSPEC:
		strprint(retv, inlen, "%scgroup", sep);
		break;
	case LXC_AUTO_CGROUP_MIXED:
		strprint(retv, inlen, "%scgroup:mixed", sep);
		break;
	case LXC_AUTO_CGROUP_RO:
		strprint(retv, inlen, "%scgroup:ro", sep);
		break;
	case LXC_AUTO_CGROUP_RW:
		strprint(retv, inlen, "%scgroup:rw", sep);
		break;
	case LXC_AUTO_CGROUP_FULL_NOSPEC:
		strprint(retv, inlen, "%scgroup-full", sep);
		break;
	case LXC_AUTO_CGROUP_FULL_MIXED:
		strprint(retv, inlen, "%scgroup-full:mixed", sep);
		break;
	case LXC_AUTO_CGROUP_FULL_RO:
		strprint(retv, inlen, "%scgroup-full:ro", sep);
		break;
	case LXC_AUTO_CGROUP_FULL_RW:
		strprint(retv, inlen, "%scgroup-full:rw", sep);
		break;
	default:
		break;
	}

	return fulllen;
}

static int get_config_mount(const char *key, char *retv, int inlen,
			    struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->mount_list) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_rootfs_path(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->rootfs.path);
}

static int get_config_rootfs_managed(const char *key, char *retv, int inlen,
				     struct lxc_conf *c, void *data)
{
	return lxc_get_conf_bool(c, retv, inlen, c->rootfs.managed);
}

static int get_config_rootfs_mount(const char *key, char *retv, int inlen,
				   struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->rootfs.mount);
}

static int get_config_rootfs_options(const char *key, char *retv, int inlen,
				     struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->rootfs.options);
}

static int get_config_uts_name(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(
	    retv, inlen,
	    c->utsname ? c->utsname->nodename : NULL);
}

static int get_config_hooks(const char *key, char *retv, int inlen,
			    struct lxc_conf *c, void *data)
{
	char *subkey;
	int len, fulllen = 0, found = -1;
	struct lxc_list *it;
	int i;

	subkey = strchr(key, '.');
	if (!subkey)
		return ret_errno(EINVAL);

	subkey = strchr(subkey + 1, '.');
	if (!subkey)
		return ret_errno(EINVAL);
	subkey++;
	if (*subkey == '\0')
		return ret_errno(EINVAL);

	for (i = 0; i < NUM_LXC_HOOKS; i++) {
		if (strcmp(lxchook_names[i], subkey) == 0) {
			found = i;
			break;
		}
	}

	if (found == -1)
		return ret_errno(EINVAL);

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->hooks[found]) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_hooks_version(const char *key, char *retv, int inlen,
				    struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->hooks_version);
}

static int get_config_net(const char *key, char *retv, int inlen,
			  struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->network) {
		struct lxc_netdev *n = it->elem;
		const char *t = lxc_net_type_to_str(n->type);
		strprint(retv, inlen, "%s\n", t ? t : "(invalid)");
	}

	return fulllen;
}

static int get_config_cap_drop(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->caps) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_cap_keep(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->keepcaps) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_console_path(const char *key, char *retv, int inlen,
				   struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->console.path);
}

static int get_config_console_logfile(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->console.log_path);
}

static int get_config_console_rotate(const char *key, char *retv, int inlen,
				     struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->console.log_rotate);
}


static int get_config_console_buffer_size(const char *key, char *retv,
					  int inlen, struct lxc_conf *c,
					  void *data)
{
	return lxc_get_conf_uint64(c, retv, inlen, c->console.buffer_size);
}

static int get_config_console_size(const char *key, char *retv, int inlen,
				   struct lxc_conf *c, void *data)
{
	return lxc_get_conf_uint64(c, retv, inlen, c->console.log_size);
}

static int get_config_seccomp_allow_nesting(const char *key, char *retv,
					    int inlen, struct lxc_conf *c,
					    void *data)
{
#ifdef HAVE_SECCOMP
	return lxc_get_conf_int(c, retv, inlen, c->seccomp.allow_nesting);
#else
	return ret_errno(ENOSYS);
#endif
}

static int get_config_seccomp_notify_cookie(const char *key, char *retv, int inlen,
					    struct lxc_conf *c, void *data)
{
#ifdef HAVE_SECCOMP_NOTIFY
	return lxc_get_conf_str(retv, inlen, c->seccomp.notifier.cookie);
#else
	return ret_errno(ENOSYS);
#endif
}

static int get_config_seccomp_notify_proxy(const char *key, char *retv, int inlen,
					   struct lxc_conf *c, void *data)
{
#ifdef HAVE_SECCOMP_NOTIFY
	return lxc_get_conf_str(retv, inlen,
				(c->seccomp.notifier.proxy_addr.sun_path[0]) == '/'
				    ? &c->seccomp.notifier.proxy_addr.sun_path[0]
				    : &c->seccomp.notifier.proxy_addr.sun_path[1]);
#else
	return ret_errno(ENOSYS);
#endif
}

static int get_config_seccomp_profile(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->seccomp.seccomp);
}

static int get_config_autodev(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->autodev);
}

static int get_config_autodev_tmpfs_size(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->autodevtmpfssize);
}

static int get_config_signal_halt(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->haltsignal);
}

static int get_config_signal_reboot(const char *key, char *retv, int inlen,
				    struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->rebootsignal);
}

static int get_config_signal_stop(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->stopsignal);
}

static int get_config_start(const char *key, char *retv, int inlen,
			    struct lxc_conf *c, void *data)
{
	if (strcmp(key + 10, "auto") == 0)
		return lxc_get_conf_int(c, retv, inlen, c->start_auto);
	else if (strcmp(key + 10, "delay") == 0)
		return lxc_get_conf_int(c, retv, inlen, c->start_delay);
	else if (strcmp(key + 10, "order") == 0)
		return lxc_get_conf_int(c, retv, inlen, c->start_order);

	return -1;
}

static int get_config_log_syslog(const char *key, char *retv, int inlen,
				 struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->syslog);
}

static int get_config_monitor(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->monitor_unshare);
}

static int get_config_monitor_signal_pdeath(const char *key, char *retv,
					    int inlen, struct lxc_conf *c,
					    void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->monitor_signal_pdeath);
}

static int get_config_group(const char *key, char *retv, int inlen,
			    struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->groups) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_environment(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	int len, fulllen = 0;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	lxc_list_for_each(it, &c->environment) {
		strprint(retv, inlen, "%s\n", (char *)it->elem);
	}

	return fulllen;
}

static int get_config_execute_cmd(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->execute_cmd);
}

static int get_config_init_cmd(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->init_cmd);
}

static int get_config_init_cwd(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	return lxc_get_conf_str(retv, inlen, c->init_cwd);
}

static int get_config_init_uid(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->init_uid);
}

static int get_config_init_gid(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->init_gid);
}

static int get_config_ephemeral(const char *key, char *retv, int inlen,
				struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->ephemeral);
}

static int get_config_no_new_privs(const char *key, char *retv, int inlen,
				   struct lxc_conf *c, void *data)
{
	return lxc_get_conf_int(c, retv, inlen, c->no_new_privs);
}

/* If you ask for a specific value, i.e. lxc.prlimit.nofile, then just the value
 * will be printed. If you ask for 'lxc.prlimit', then all limit entries will be
 * printed, in 'lxc.prlimit.resource = value' format.
 */
static int get_config_prlimit(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	int fulllen = 0, len;
	bool get_all = false;
	struct lxc_list *it;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!strcmp(key, "lxc.prlimit"))
		get_all = true;
	else if (strncmp(key, "lxc.prlimit.", 12) == 0)
		key += 12;
	else
		return ret_errno(EINVAL);

	lxc_list_for_each(it, &c->limits) {
		/* 2 colon separated 64 bit integers or the word 'unlimited' */
		char buf[INTTYPE_TO_STRLEN(uint64_t) * 2 + 2];
		int partlen;
		struct lxc_limit *lim = it->elem;

		if (lim->limit.rlim_cur == RLIM_INFINITY) {
			memcpy(buf, "unlimited", STRLITERALLEN("unlimited") + 1);
			partlen = STRLITERALLEN("unlimited");
		} else {
			partlen = sprintf(buf, "%" PRIu64,
					  (uint64_t)lim->limit.rlim_cur);
		}

		if (lim->limit.rlim_cur != lim->limit.rlim_max) {
			if (lim->limit.rlim_max == RLIM_INFINITY)
				memcpy(buf + partlen, ":unlimited",
				       STRLITERALLEN(":unlimited") + 1);
			else
				sprintf(buf + partlen, ":%" PRIu64,
					(uint64_t)lim->limit.rlim_max);
		}

		if (get_all) {
			strprint(retv, inlen, "lxc.prlimit.%s = %s\n",
				 lim->resource, buf);
		} else if (strcmp(lim->resource, key) == 0) {
			strprint(retv, inlen, "%s", buf);
		}
	}

	return fulllen;
}

/* If you ask for a specific value, i.e. lxc.sysctl.net.ipv4.ip_forward, then
 * just the value will be printed. If you ask for 'lxc.sysctl', then all sysctl
 * entries will be printed, in 'lxc.sysctl.key = value' format.
 */
static int get_config_sysctl(const char *key, char *retv, int inlen,
			     struct lxc_conf *c, void *data)
{
	int len;
	struct lxc_list *it;
	int fulllen = 0;
	bool get_all = false;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (strcmp(key, "lxc.sysctl") == 0)
		get_all = true;
	else if (strncmp(key, "lxc.sysctl.", STRLITERALLEN("lxc.sysctl.")) == 0)
		key += STRLITERALLEN("lxc.sysctl.");
	else
		return ret_errno(EINVAL);

	lxc_list_for_each(it, &c->sysctls) {
		struct lxc_sysctl *elem = it->elem;
		if (get_all) {
			strprint(retv, inlen, "lxc.sysctl.%s = %s\n", elem->key,
				 elem->value);
		} else if (strcmp(elem->key, key) == 0) {
			strprint(retv, inlen, "%s", elem->value);
		}
	}

	return fulllen;
}

static int get_config_proc(const char *key, char *retv, int inlen,
			   struct lxc_conf *c, void *data)
{
	struct lxc_list *it;
	int len;
	int fulllen = 0;
	bool get_all = false;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (strcmp(key, "lxc.proc") == 0)
		get_all = true;
	else if (strncmp(key, "lxc.proc.", STRLITERALLEN("lxc.proc.")) == 0)
		key += STRLITERALLEN("lxc.proc.");
	else
		return ret_errno(EINVAL);

	lxc_list_for_each(it, &c->procs) {
		struct lxc_proc *proc = it->elem;

		if (get_all) {
			strprint(retv, inlen, "lxc.proc.%s = %s\n",
			         proc->filename, proc->value);
		} else if (strcmp(proc->filename, key) == 0) {
			strprint(retv, inlen, "%s", proc->value);
		}
	}

	return fulllen;
}

static int get_config_namespace_clone(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	int i, len;
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	for (i = 0; i < LXC_NS_MAX; i++) {
		if (c->ns_clone & ns_info[i].clone_flag)
			strprint(retv, inlen, "%s\n", ns_info[i].proc_name);
	}

	return fulllen;
}

static int get_config_namespace_keep(const char *key, char *retv, int inlen,
				     struct lxc_conf *c, void *data)
{
	int i, len;
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	for (i = 0; i < LXC_NS_MAX; i++) {
		if (c->ns_keep & ns_info[i].clone_flag)
			strprint(retv, inlen, "%s\n", ns_info[i].proc_name);
	}

	return fulllen;
}

static int get_config_namespace_share(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	int len, ns_idx;
	const char *namespace;
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	namespace = key + STRLITERALLEN("lxc.namespace.share.");
	ns_idx = lxc_namespace_2_ns_idx(namespace);
	if (ns_idx < 0)
		return ns_idx;

	strprint(retv, inlen, "%s", c->ns_share[ns_idx]);

	return fulllen;
}

/* Callbacks to clear config items. */
static inline int clr_config_personality(const char *key, struct lxc_conf *c,
					 void *data)
{
	c->personality = -1;
	return 0;
}

static inline int clr_config_pty_max(const char *key, struct lxc_conf *c,
				     void *data)
{
	c->pty_max = 0;
	return 0;
}

static inline int clr_config_tty_max(const char *key, struct lxc_conf *c,
				     void *data)
{
	c->ttys.tty = 0;
	return 0;
}

static inline int clr_config_tty_dir(const char *key, struct lxc_conf *c,
				    void *data)
{
	free_disarm(c->ttys.dir);
	return 0;
}

static inline int clr_config_apparmor_profile(const char *key,
					      struct lxc_conf *c, void *data)
{
	free_disarm(c->lsm_aa_profile);
	return 0;
}

static inline int clr_config_apparmor_allow_incomplete(const char *key,
						       struct lxc_conf *c,
						       void *data)
{
	c->lsm_aa_allow_incomplete = 0;
	return 0;
}

static inline int clr_config_apparmor_allow_nesting(const char *key,
						    struct lxc_conf *c,
						    void *data)
{
	c->lsm_aa_allow_nesting = 0;
	return 0;
}

static inline int clr_config_apparmor_raw(const char *key,
					  struct lxc_conf *c,
					  void *data)
{
	return lxc_clear_apparmor_raw(c);
}

static inline int clr_config_selinux_context(const char *key,
					     struct lxc_conf *c, void *data)
{
	free_disarm(c->lsm_se_context);
	return 0;
}

static inline int clr_config_selinux_context_keyring(const char *key,
						     struct lxc_conf *c, void *data)
{
	free_disarm(c->lsm_se_keyring_context);
	return 0;
}

static inline int clr_config_keyring_session(const char *key,
					     struct lxc_conf *c, void *data)
{
	c->keyring_disable_session = false;
	return 0;
}

static inline int clr_config_cgroup_controller(const char *key,
					       struct lxc_conf *c, void *data)
{
	return lxc_clear_cgroups(c, key, CGROUP_SUPER_MAGIC);
}

static inline int clr_config_cgroup2_controller(const char *key,
						struct lxc_conf *c, void *data)
{
	return lxc_clear_cgroups(c, key, CGROUP2_SUPER_MAGIC);
}

static int clr_config_cgroup_dir(const char *key, struct lxc_conf *lxc_conf,
				 void *data)
{
	if (strcmp(key, "lxc.cgroup.dir") != 0)
		return ret_errno(EINVAL);

	if (lxc_conf->cgroup_meta.dir)
		free_disarm(lxc_conf->cgroup_meta.dir);

	return 0;
}

static inline int clr_config_cgroup_relative(const char *key,
					     struct lxc_conf *lxc_conf,
					     void *data)
{
	lxc_conf->cgroup_meta.relative = false;
	return 0;
}

static inline int clr_config_idmaps(const char *key, struct lxc_conf *c,
				    void *data)
{
	return lxc_clear_idmaps(c);
}

static inline int clr_config_log_level(const char *key, struct lxc_conf *c,
				      void *data)
{
	c->loglevel = LXC_LOG_LEVEL_NOTSET;
	return 0;
}

static inline int clr_config_log_file(const char *key, struct lxc_conf *c,
				     void *data)
{
	free_disarm(c->logfile);
	return 0;
}

static inline int clr_config_mount(const char *key, struct lxc_conf *c,
				   void *data)
{
	return lxc_clear_mount_entries(c);
}

static inline int clr_config_mount_auto(const char *key, struct lxc_conf *c,
					void *data)
{
	return lxc_clear_automounts(c);
}

static inline int clr_config_mount_fstab(const char *key, struct lxc_conf *c,
					 void *data)
{
	free_disarm(c->fstab);
	return 0;
}

static inline int clr_config_rootfs_path(const char *key, struct lxc_conf *c,
					 void *data)
{
	free_disarm(c->rootfs.path);
	return 0;
}

static inline int clr_config_rootfs_managed(const char *key, struct lxc_conf *c,
					    void *data)
{
	c->rootfs.managed = true;
	return 0;
}

static inline int clr_config_rootfs_mount(const char *key, struct lxc_conf *c,
					  void *data)
{
	free_disarm(c->rootfs.mount);
	return 0;
}

static inline int clr_config_rootfs_options(const char *key, struct lxc_conf *c,
					    void *data)
{
	free_disarm(c->rootfs.options);
	free_disarm(c->rootfs.data);

	return 0;
}

static inline int clr_config_uts_name(const char *key, struct lxc_conf *c,
				     void *data)
{
	free_disarm(c->utsname);
	return 0;
}

static inline int clr_config_hooks(const char *key, struct lxc_conf *c,
				   void *data)
{
	return lxc_clear_hooks(c, key);
}

static inline int clr_config_hooks_version(const char *key, struct lxc_conf *c,
					   void *data)
{
	/* default to legacy hooks version */
	c->hooks_version = 0;
	return 0;
}

static inline int clr_config_net(const char *key, struct lxc_conf *c,
				 void *data)
{
	lxc_free_networks(&c->network);

	return 0;
}

static inline int clr_config_cap_drop(const char *key, struct lxc_conf *c,
				      void *data)
{
	return lxc_clear_config_caps(c);
}

static inline int clr_config_cap_keep(const char *key, struct lxc_conf *c,
				      void *data)
{
	return lxc_clear_config_keepcaps(c);
}

static inline int clr_config_console_path(const char *key, struct lxc_conf *c,
					  void *data)
{
	free_disarm(c->console.path);
	return 0;
}

static inline int clr_config_console_logfile(const char *key,
					     struct lxc_conf *c, void *data)
{
	free_disarm(c->console.log_path);
	return 0;
}

static inline int clr_config_console_rotate(const char *key, struct lxc_conf *c,
					    void *data)
{
	c->console.log_rotate = 0;
	return 0;
}

static inline int clr_config_console_buffer_size(const char *key,
						 struct lxc_conf *c, void *data)
{
	c->console.buffer_size = 0;
	return 0;
}

static inline int clr_config_console_size(const char *key, struct lxc_conf *c,
					  void *data)
{
	c->console.log_size = 0;
	return 0;
}

static inline int clr_config_seccomp_allow_nesting(const char *key,
						   struct lxc_conf *c, void *data)
{
#ifdef HAVE_SECCOMP
	c->seccomp.allow_nesting = 0;
	return 0;
#else
	return ret_errno(ENOSYS);
#endif
}

static inline int clr_config_seccomp_notify_cookie(const char *key,
						   struct lxc_conf *c, void *data)
{
#ifdef HAVE_SECCOMP_NOTIFY
	free_disarm(c->seccomp.notifier.cookie);
	return 0;
#else
	return ret_errno(ENOSYS);
#endif
}

static inline int clr_config_seccomp_notify_proxy(const char *key,
						   struct lxc_conf *c, void *data)
{
#ifdef HAVE_SECCOMP_NOTIFY
	memset(&c->seccomp.notifier.proxy_addr, 0,
	       sizeof(c->seccomp.notifier.proxy_addr));
	return 0;
#else
	return ret_errno(ENOSYS);
#endif
}

static inline int clr_config_seccomp_profile(const char *key,
					     struct lxc_conf *c, void *data)
{
	free_disarm(c->seccomp.seccomp);
	return 0;
}

static inline int clr_config_autodev(const char *key, struct lxc_conf *c,
				     void *data)
{
	c->autodev = 1;
	return 0;
}

static inline int clr_config_autodev_tmpfs_size(const char *key, struct lxc_conf *c,
				     void *data)
{
	c->autodevtmpfssize = 500000;
	return 0;
}

static inline int clr_config_signal_halt(const char *key, struct lxc_conf *c,
					void *data)
{
	c->haltsignal = 0;
	return 0;
}

static inline int clr_config_signal_reboot(const char *key, struct lxc_conf *c,
					  void *data)
{
	c->rebootsignal = 0;
	return 0;
}

static inline int clr_config_signal_stop(const char *key, struct lxc_conf *c,
					void *data)
{
	c->stopsignal = 0;
	return 0;
}

static inline int clr_config_start(const char *key, struct lxc_conf *c,
				   void *data)
{
	if (strcmp(key + 10, "auto") == 0)
		c->start_auto = 0;
	else if (strcmp(key + 10, "delay") == 0)
		c->start_delay = 0;
	else if (strcmp(key + 10, "order") == 0)
		c->start_order = 0;

	return 0;
}

static inline int clr_config_log_syslog(const char *key, struct lxc_conf *c,
				    void *data)
{
	free_disarm(c->syslog);
	return 0;
}

static inline int clr_config_monitor(const char *key, struct lxc_conf *c,
				     void *data)
{
	c->monitor_unshare = 0;
	return 0;
}

static inline int clr_config_monitor_signal_pdeath(const char *key,
						   struct lxc_conf *c, void *data)
{
	c->monitor_signal_pdeath = 0;
	return 0;
}

static inline int clr_config_group(const char *key, struct lxc_conf *c,
				   void *data)
{
	return lxc_clear_groups(c);
}

static inline int clr_config_environment(const char *key, struct lxc_conf *c,
					 void *data)
{
	return lxc_clear_environment(c);
}

static inline int clr_config_execute_cmd(const char *key, struct lxc_conf *c,
				      void *data)
{
	free_disarm(c->execute_cmd);
	return 0;
}

static inline int clr_config_init_cmd(const char *key, struct lxc_conf *c,
				      void *data)
{
	free_disarm(c->init_cmd);
	return 0;
}

static inline int clr_config_init_cwd(const char *key, struct lxc_conf *c,
				      void *data)
{
	free_disarm(c->init_cwd);
	return 0;
}

static inline int clr_config_init_uid(const char *key, struct lxc_conf *c,
				      void *data)
{
	c->init_uid = 0;
	return 0;
}

static inline int clr_config_init_gid(const char *key, struct lxc_conf *c,
				      void *data)
{
	c->init_gid = 0;
	return 0;
}

static inline int clr_config_ephemeral(const char *key, struct lxc_conf *c,
				       void *data)
{
	c->ephemeral = 0;
	return 0;
}

static inline int clr_config_no_new_privs(const char *key, struct lxc_conf *c,
					  void *data)
{
	c->no_new_privs = false;
	return 0;
}

static inline int clr_config_prlimit(const char *key, struct lxc_conf *c,
				   void *data)
{
	return lxc_clear_limits(c, key);
}

static inline int clr_config_sysctl(const char *key, struct lxc_conf *c,
				   void *data)
{
	return lxc_clear_sysctls(c, key);
}

static inline int clr_config_proc(const char *key, struct lxc_conf *c,
				   void *data)
{
	return lxc_clear_procs(c, key);
}

static inline int clr_config_includefiles(const char *key, struct lxc_conf *c,
					  void *data)
{
	lxc_clear_includes(c);
	return 0;
}

static int clr_config_namespace_clone(const char *key,
				      struct lxc_conf *lxc_conf, void *data)
{
	lxc_conf->ns_clone = 0;
	return 0;
}

static int clr_config_namespace_keep(const char *key, struct lxc_conf *lxc_conf,
				     void *data)
{
	lxc_conf->ns_keep = 0;
	return 0;
}

static int clr_config_namespace_share(const char *key,
				      struct lxc_conf *lxc_conf, void *data)
{
	int ns_idx;
	const char *namespace;

	namespace = key + STRLITERALLEN("lxc.namespace.share.");
	ns_idx = lxc_namespace_2_ns_idx(namespace);
	if (ns_idx < 0)
		return ns_idx;

	free(lxc_conf->ns_share[ns_idx]);
	lxc_conf->ns_share[ns_idx] = NULL;

	return 0;
}

static int get_config_includefiles(const char *key, char *retv, int inlen,
				   struct lxc_conf *c, void *data)
{
	return ret_errno(ENOSYS);
}

static struct lxc_config_t *get_network_config_ops(const char *key,
						   struct lxc_conf *lxc_conf,
						   ssize_t *idx,
						   char **deindexed_key)
{
	__do_free char *copy = NULL;
	struct lxc_config_t *config = NULL;
	int ret;
	unsigned int tmpidx;
	size_t numstrlen;
	char *idx_start, *idx_end;

	/* check that this is a sensible network key */
	if (strncmp("lxc.net.", key, 8))
		return log_error_errno(NULL, EINVAL, "Invalid network configuration key \"%s\"", key);

	copy = strdup(key);
	if (!copy)
		return log_error_errno(NULL, ENOMEM, "Failed to duplicate string \"%s\"", key);

	/* lxc.net.<n> */
	if (!isdigit(*(key + 8)))
		return log_error_errno(NULL, EINVAL, "Failed to detect digit in string \"%s\"", key + 8);

	/* beginning of index string */
	idx_start = (copy + 7);
	*idx_start = '\0';

	/* end of index string */
	idx_end = strchr((copy + 8), '.');
	if (idx_end)
		*idx_end = '\0';

	/* parse current index */
	ret = lxc_safe_uint((idx_start + 1), &tmpidx);
	if (ret < 0) {
		*idx = ret;
		return log_error_errno(NULL, -ret, "Failed to parse unsigned integer from string \"%s\"", idx_start + 1);
	}

	/* This, of course is utterly nonsensical on so many levels, but
	 * better safe than sorry.
	 * (Checking for INT_MAX here is intentional.)
	 */
	if (tmpidx == INT_MAX)
		return log_error_errno(NULL, ERANGE, "Number of configured networks would overflow the counter");
	*idx = tmpidx;

	numstrlen = strlen((idx_start + 1));

	/* repair configuration key */
	*idx_start = '.';

	/* lxc.net.<idx>.<subkey> */
	if (idx_end) {
		*idx_end = '.';
		if (strlen(idx_end + 1) == 0)
			return log_error_errno(NULL, EINVAL, "No subkey in network configuration key \"%s\"", key);

		memmove(copy + 8, idx_end + 1, strlen(idx_end + 1));
		copy[strlen(key) - (numstrlen + 1)] = '\0';

		config = lxc_get_config(copy);
		if (!config)
			return log_error_errno(NULL, ENOENT, "Unknown network configuration key \"%s\"", key);
	}

	if (deindexed_key)
		*deindexed_key = move_ptr(copy);

	return config;
}

/* Config entry is something like "lxc.net.0.ipv4" the key 'lxc.net.' was
 * found. So we make sure next comes an integer, find the right callback (by
 * rewriting the key), and call it.
 */
static int set_config_net_nic(const char *key, const char *value,
			      struct lxc_conf *lxc_conf, void *data)
{
	__do_free char *deindexed_key = NULL;
	ssize_t idx = -1;
	const char *idxstring;
	struct lxc_config_t *config;
	struct lxc_netdev *netdev;

	idxstring = key + 8;
	if (!isdigit(*idxstring))
		return ret_errno(EINVAL);

	if (lxc_config_value_empty(value))
		return clr_config_net_nic(key, lxc_conf, data);

	config = get_network_config_ops(key, lxc_conf, &idx, &deindexed_key);
	if (!config || idx < 0)
		return -errno;

	netdev = lxc_get_netdev_by_idx(lxc_conf, (unsigned int)idx, true);
	if (!netdev)
		return ret_errno(EINVAL);

	return config->set(deindexed_key, value, lxc_conf, netdev);
}

static int clr_config_net_nic(const char *key, struct lxc_conf *lxc_conf,
			      void *data)
{
	__do_free char *deindexed_key = NULL;
	ssize_t idx = -1;
	int ret;
	const char *idxstring;
	struct lxc_config_t *config;
	struct lxc_netdev *netdev;

	idxstring = key + 8;
	if (!isdigit(*idxstring))
		return ret_errno(EINVAL);

	/* The left conjunct is pretty self-explanatory. The right conjunct
	 * checks whether the two pointers are equal. If they are we know that
	 * this is not a key that is namespaced any further and so we are
	 * supposed to clear the whole network.
	 */
	if (isdigit(*idxstring) && (strrchr(key, '.') == (idxstring - 1))) {
		unsigned int rmnetdevidx;

		ret = lxc_safe_uint(idxstring, &rmnetdevidx);
		if (ret < 0)
			return ret;

		/* Remove network from network list. */
		lxc_remove_nic_by_idx(lxc_conf, rmnetdevidx);
		return 0;
	}

	config = get_network_config_ops(key, lxc_conf, &idx, &deindexed_key);
	if (!config || idx < 0)
		return -errno;

	netdev = lxc_get_netdev_by_idx(lxc_conf, (unsigned int)idx, false);
	if (!netdev)
		return ret_errno(EINVAL);

	return config->clr(deindexed_key, lxc_conf, netdev);
}

static int clr_config_net_type(const char *key, struct lxc_conf *lxc_conf,
			       void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->type = -1;

	return 0;
}

static int clr_config_net_name(const char *key, struct lxc_conf *lxc_conf,
			       void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->name[0] = '\0';

	return 0;
}

static int clr_config_net_flags(const char *key, struct lxc_conf *lxc_conf,
				void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->flags = 0;

	return 0;
}

static int clr_config_net_link(const char *key, struct lxc_conf *lxc_conf,
			       void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->link[0] = '\0';

	return 0;
}

static int clr_config_net_l2proxy(const char *key, struct lxc_conf *lxc_conf,
			       void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->l2proxy = false;

	return 0;
}

static int clr_config_net_macvlan_mode(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_MACVLAN)
		return 0;

	netdev->priv.macvlan_attr.mode = -1;

	return 0;
}

static int clr_config_net_ipvlan_mode(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_IPVLAN)
		return 0;

	netdev->priv.ipvlan_attr.mode = -1;

	return 0;
}

static int clr_config_net_ipvlan_isolation(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_IPVLAN)
		return 0;

	netdev->priv.ipvlan_attr.isolation = -1;

	return 0;
}

static int clr_config_net_veth_mode(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return 0;

	netdev->priv.veth_attr.mode = -1;

	return 0;
}

static int clr_config_net_veth_pair(const char *key, struct lxc_conf *lxc_conf,
				    void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->priv.veth_attr.pair[0] = '\0';

	return 0;
}

static int clr_config_net_script_up(const char *key, struct lxc_conf *lxc_conf,
				    void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	free_disarm(netdev->upscript);

	return 0;
}

static int clr_config_net_script_down(const char *key,
				      struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	free_disarm(netdev->downscript);

	return 0;
}

static int clr_config_net_hwaddr(const char *key, struct lxc_conf *lxc_conf,
				 void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	free_disarm(netdev->hwaddr);

	return 0;
}

static int clr_config_net_mtu(const char *key, struct lxc_conf *lxc_conf,
			      void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	free_disarm(netdev->mtu);

	return 0;
}

static int clr_config_net_vlan_id(const char *key, struct lxc_conf *lxc_conf,
				  void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	netdev->priv.vlan_attr.vid = 0;

	return 0;
}

static int clr_config_net_ipv4_gateway(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	free_disarm(netdev->ipv4_gateway);

	return 0;
}

static int clr_config_net_ipv4_address(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	struct lxc_list *cur, *next;

	if (!netdev)
		return ret_errno(EINVAL);

	lxc_list_for_each_safe(cur, &netdev->ipv4, next) {
		lxc_list_del(cur);
		free(cur->elem);
		free(cur);
	}

	return 0;
}

static int clr_config_net_veth_ipv4_route(const char *key,
					  struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	struct lxc_list *cur, *next;

	if (!netdev)
		return ret_errno(EINVAL);

	lxc_list_for_each_safe(cur, &netdev->priv.veth_attr.ipv4_routes, next) {
		lxc_list_del(cur);
		free(cur->elem);
		free(cur);
	}

	return 0;
}

static int clr_config_net_ipv6_gateway(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;

	if (!netdev)
		return ret_errno(EINVAL);

	free_disarm(netdev->ipv6_gateway);

	return 0;
}

static int clr_config_net_ipv6_address(const char *key,
				       struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	struct lxc_list *cur, *next;

	if (!netdev)
		return ret_errno(EINVAL);

	lxc_list_for_each_safe(cur, &netdev->ipv6, next) {
		lxc_list_del(cur);
		free(cur->elem);
		free(cur);
	}

	return 0;
}

static int clr_config_net_veth_ipv6_route(const char *key,
					  struct lxc_conf *lxc_conf, void *data)
{
	struct lxc_netdev *netdev = data;
	struct lxc_list *cur, *next;

	if (!netdev)
		return ret_errno(EINVAL);

	lxc_list_for_each_safe(cur, &netdev->priv.veth_attr.ipv6_routes, next) {
		lxc_list_del(cur);
		free(cur->elem);
		free(cur);
	}

	return 0;
}

static int get_config_net_nic(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	__do_free char *deindexed_key = NULL;
	ssize_t idx = -1;
	const char *idxstring;
	struct lxc_config_t *config;
	struct lxc_netdev *netdev;

	idxstring = key + 8;
	if (!isdigit(*idxstring))
		return ret_errno(EINVAL);

	config = get_network_config_ops(key, c, &idx, &deindexed_key);
	if (!config || idx < 0)
		return -errno;

	netdev = lxc_get_netdev_by_idx(c, (unsigned int)idx, false);
	if (!netdev)
		return ret_errno(EINVAL);

	return config->get(deindexed_key, retv, inlen, c, netdev);
}

static int get_config_net_type(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	strprint(retv, inlen, "%s", lxc_net_type_to_str(netdev->type));

	return fulllen;
}

static int get_config_net_flags(const char *key, char *retv, int inlen,
				struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->flags & IFF_UP)
		strprint(retv, inlen, "up");

	return fulllen;
}

static int get_config_net_link(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->link[0] != '\0')
		strprint(retv, inlen, "%s", netdev->link);

	return fulllen;
}

static int get_config_net_l2proxy(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	struct lxc_netdev *netdev = data;
	return lxc_get_conf_bool(c, retv, inlen, netdev->l2proxy);
}

static int get_config_net_name(const char *key, char *retv, int inlen,
			       struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->name[0] != '\0')
		strprint(retv, inlen, "%s", netdev->name);

	return fulllen;
}

static int get_config_net_macvlan_mode(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	const char *mode;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_MACVLAN)
		return 0;

	switch (netdev->priv.macvlan_attr.mode) {
	case MACVLAN_MODE_PRIVATE:
		mode = "private";
		break;
	case MACVLAN_MODE_VEPA:
		mode = "vepa";
		break;
	case MACVLAN_MODE_BRIDGE:
		mode = "bridge";
		break;
	case MACVLAN_MODE_PASSTHRU:
		mode = "passthru";
		break;
	default:
		mode = "(invalid)";
		break;
	}

	strprint(retv, inlen, "%s", mode);

	return fulllen;
}

static int get_config_net_ipvlan_mode(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int fulllen = 0;
	struct lxc_netdev *netdev = data;
	int len;
	const char *mode;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_IPVLAN)
		return 0;

	switch (netdev->priv.ipvlan_attr.mode) {
	case IPVLAN_MODE_L3:
		mode = "l3";
		break;
	case IPVLAN_MODE_L3S:
		mode = "l3s";
		break;
	case IPVLAN_MODE_L2:
		mode = "l2";
		break;
	default:
		mode = "(invalid)";
		break;
	}

	strprint(retv, inlen, "%s", mode);

	return fulllen;
}

static int get_config_net_ipvlan_isolation(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int fulllen = 0;
	struct lxc_netdev *netdev = data;
	int len;
	const char *mode;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_IPVLAN)
		return 0;

	switch (netdev->priv.ipvlan_attr.isolation) {
	case IPVLAN_ISOLATION_BRIDGE:
		mode = "bridge";
		break;
	case IPVLAN_ISOLATION_PRIVATE:
		mode = "private";
		break;
	case IPVLAN_ISOLATION_VEPA:
		mode = "vepa";
		break;
	default:
		mode = "(invalid)";
		break;
	}

	strprint(retv, inlen, "%s", mode);

	return fulllen;
}

static int get_config_net_veth_mode(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int fulllen = 0;
	struct lxc_netdev *netdev = data;
	int len;
	const char *mode;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return 0;

	switch (netdev->priv.veth_attr.mode) {
	case VETH_MODE_BRIDGE:
		mode = "bridge";
		break;
	case VETH_MODE_ROUTER:
		mode = "router";
		break;
	default:
		mode = "(invalid)";
		break;
	}

	strprint(retv, inlen, "%s", mode);

	return fulllen;
}

static int get_config_net_veth_pair(const char *key, char *retv, int inlen,
				    struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return 0;

	strprint(retv, inlen, "%s",
		 netdev->priv.veth_attr.pair[0] != '\0'
		     ? netdev->priv.veth_attr.pair
		     : netdev->priv.veth_attr.veth1);

	return fulllen;
}

static int get_config_net_script_up(const char *key, char *retv, int inlen,
				    struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->upscript)
		strprint(retv, inlen, "%s", netdev->upscript);

	return fulllen;
}

static int get_config_net_script_down(const char *key, char *retv, int inlen,
				      struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->downscript)
		strprint(retv, inlen, "%s", netdev->downscript);

	return fulllen;
}

static int get_config_net_hwaddr(const char *key, char *retv, int inlen,
				 struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->hwaddr)
		strprint(retv, inlen, "%s", netdev->hwaddr);

	return fulllen;
}

static int get_config_net_mtu(const char *key, char *retv, int inlen,
			      struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->mtu)
		strprint(retv, inlen, "%s", netdev->mtu);

	return fulllen;
}

static int get_config_net_vlan_id(const char *key, char *retv, int inlen,
				  struct lxc_conf *c, void *data)
{
	int len;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VLAN)
		return 0;

	strprint(retv, inlen, "%d", netdev->priv.vlan_attr.vid);

	return fulllen;
}

static int get_config_net_ipv4_gateway(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int len;
	char buf[INET_ADDRSTRLEN];
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->ipv4_gateway_auto) {
		strprint(retv, inlen, "auto");
	} else if (netdev->ipv4_gateway_dev) {
		strprint(retv, inlen, "dev");
	} else if (netdev->ipv4_gateway) {
		if (!inet_ntop(AF_INET, netdev->ipv4_gateway, buf, sizeof(buf)))
			return -errno;
		strprint(retv, inlen, "%s", buf);
	}

	return fulllen;
}

static int get_config_net_ipv4_address(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int len;
	size_t listlen;
	char buf[INET_ADDRSTRLEN];
	struct lxc_list *it;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	listlen = lxc_list_len(&netdev->ipv4);

	lxc_list_for_each(it, &netdev->ipv4) {
		struct lxc_inetdev *i = it->elem;
		if (!inet_ntop(AF_INET, &i->addr, buf, sizeof(buf)))
			return -errno;
		strprint(retv, inlen, "%s/%u%s", buf, i->prefix,
			 (listlen-- > 1) ? "\n" : "");
	}

	return fulllen;
}

static int get_config_net_veth_ipv4_route(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int len;
	size_t listlen;
	char buf[INET_ADDRSTRLEN];
	struct lxc_list *it;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return 0;

	listlen = lxc_list_len(&netdev->priv.veth_attr.ipv4_routes);

	lxc_list_for_each(it, &netdev->priv.veth_attr.ipv4_routes) {
		struct lxc_inetdev *i = it->elem;
		if (!inet_ntop(AF_INET, &i->addr, buf, sizeof(buf)))
			return -errno;
		strprint(retv, inlen, "%s/%u%s", buf, i->prefix,
			 (listlen-- > 1) ? "\n" : "");
	}

	return fulllen;
}

static int get_config_net_ipv6_gateway(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int len;
	char buf[INET6_ADDRSTRLEN];
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->ipv6_gateway_auto) {
		strprint(retv, inlen, "auto");
	} else if (netdev->ipv6_gateway_dev) {
		strprint(retv, inlen, "dev");
	} else if (netdev->ipv6_gateway) {
		if (!inet_ntop(AF_INET6, netdev->ipv6_gateway, buf, sizeof(buf)))
			return -errno;
		strprint(retv, inlen, "%s", buf);
	}

	return fulllen;
}

static int get_config_net_ipv6_address(const char *key, char *retv, int inlen,
				       struct lxc_conf *c, void *data)
{
	int len;
	size_t listlen;
	char buf[INET6_ADDRSTRLEN];
	struct lxc_list *it;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	listlen = lxc_list_len(&netdev->ipv6);

	lxc_list_for_each(it, &netdev->ipv6) {
		struct lxc_inet6dev *i = it->elem;
		if (!inet_ntop(AF_INET6, &i->addr, buf, sizeof(buf)))
			return -errno;
		strprint(retv, inlen, "%s/%u%s", buf, i->prefix,
			 (listlen-- > 1) ? "\n" : "");
	}

	return fulllen;
}

static int get_config_net_veth_ipv6_route(const char *key, char *retv, int inlen,
					  struct lxc_conf *c, void *data)
{
	int len;
	size_t listlen;
	char buf[INET6_ADDRSTRLEN];
	struct lxc_list *it;
	int fulllen = 0;
	struct lxc_netdev *netdev = data;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!netdev)
		return ret_errno(EINVAL);

	if (netdev->type != LXC_NET_VETH)
		return 0;

	listlen = lxc_list_len(&netdev->priv.veth_attr.ipv6_routes);

	lxc_list_for_each(it, &netdev->priv.veth_attr.ipv6_routes) {
		struct lxc_inet6dev *i = it->elem;
		if (!inet_ntop(AF_INET6, &i->addr, buf, sizeof(buf)))
			return -errno;
		strprint(retv, inlen, "%s/%u%s", buf, i->prefix,
			 (listlen-- > 1) ? "\n" : "");
	}

	return fulllen;
}

int lxc_list_config_items(char *retv, int inlen)
{
	size_t i;
	int len;
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	for (i = 0; i < config_jump_table_size; i++) {
		char *s = config_jump_table[i].name;

		if (s[strlen(s) - 1] == '.')
			continue;

		strprint(retv, inlen, "%s\n", s);
	}

	return fulllen;
}

int lxc_list_subkeys(struct lxc_conf *conf, const char *key, char *retv,
		     int inlen)
{
	int len;
	int fulllen = 0;

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	if (!strcmp(key, "lxc.apparmor")) {
		strprint(retv, inlen, "allow_incomplete\n");
		strprint(retv, inlen, "allow_nesting\n");
		strprint(retv, inlen, "profile\n");
		strprint(retv, inlen, "raw\n");
	} else if (!strcmp(key, "lxc.cgroup")) {
		strprint(retv, inlen, "dir\n");
	} else if (!strcmp(key, "lxc.selinux")) {
		strprint(retv, inlen, "context\n");
		strprint(retv, inlen, "context.keyring\n");
	} else if (!strcmp(key, "lxc.mount")) {
		strprint(retv, inlen, "auto\n");
		strprint(retv, inlen, "entry\n");
		strprint(retv, inlen, "fstab\n");
	} else if (!strcmp(key, "lxc.rootfs")) {
		strprint(retv, inlen, "mount\n");
		strprint(retv, inlen, "options\n");
		strprint(retv, inlen, "path\n");
	} else if (!strcmp(key, "lxc.uts")) {
		strprint(retv, inlen, "name\n");
	} else if (!strcmp(key, "lxc.hook")) {
		strprint(retv, inlen, "autodev\n");
		strprint(retv, inlen, "autodevtmpfssize\n");
		strprint(retv, inlen, "clone\n");
		strprint(retv, inlen, "destroy\n");
		strprint(retv, inlen, "mount\n");
		strprint(retv, inlen, "post-stop\n");
		strprint(retv, inlen, "pre-mount\n");
		strprint(retv, inlen, "pre-start\n");
		strprint(retv, inlen, "start-host\n");
		strprint(retv, inlen, "start\n");
		strprint(retv, inlen, "stop\n");
	} else if (!strcmp(key, "lxc.cap")) {
		strprint(retv, inlen, "drop\n");
		strprint(retv, inlen, "keep\n");
	} else if (!strcmp(key, "lxc.console")) {
		strprint(retv, inlen, "logfile\n");
		strprint(retv, inlen, "path\n");
	} else if (!strcmp(key, "lxc.seccomp")) {
		strprint(retv, inlen, "profile\n");
	} else if (!strcmp(key, "lxc.signal")) {
		strprint(retv, inlen, "halt\n");
		strprint(retv, inlen, "reboot\n");
		strprint(retv, inlen, "stop\n");
	} else if (!strcmp(key, "lxc.start")) {
		strprint(retv, inlen, "auto\n");
		strprint(retv, inlen, "delay\n");
		strprint(retv, inlen, "order\n");
	} else if (!strcmp(key, "lxc.monitor")) {
		strprint(retv, inlen, "unshare\n");
	} else if (!strcmp(key, "lxc.keyring")) {
		strprint(retv, inlen, "session\n");
	} else {
		fulllen = ret_errno(EINVAL);
	}

	return fulllen;
}

int lxc_list_net(struct lxc_conf *c, const char *key, char *retv, int inlen)
{
	int len;
	const char *idxstring;
	struct lxc_netdev *netdev;
	int fulllen = 0;
	ssize_t idx = -1;

	idxstring = key + 8;
	if (!isdigit(*idxstring))
		return -1;

	(void)get_network_config_ops(key, c, &idx, NULL);
	if (idx < 0)
		return ret_errno(EINVAL);

	netdev = lxc_get_netdev_by_idx(c, (unsigned int)idx, false);
	if (!netdev)
		return ret_errno(EINVAL);

	if (!retv)
		inlen = 0;
	else
		memset(retv, 0, inlen);

	strprint(retv, inlen, "type\n");
	strprint(retv, inlen, "script.up\n");
	strprint(retv, inlen, "script.down\n");

	if (netdev->type != LXC_NET_EMPTY) {
		strprint(retv, inlen, "flags\n");
		strprint(retv, inlen, "link\n");
		strprint(retv, inlen, "name\n");
		strprint(retv, inlen, "hwaddr\n");
		strprint(retv, inlen, "mtu\n");
		strprint(retv, inlen, "ipv6.address\n");
		strprint(retv, inlen, "ipv6.gateway\n");
		strprint(retv, inlen, "ipv4.address\n");
		strprint(retv, inlen, "ipv4.gateway\n");
	}

	switch (netdev->type) {
	case LXC_NET_VETH:
		strprint(retv, inlen, "veth.pair\n");
		strprint(retv, inlen, "veth.ipv4.route\n");
		strprint(retv, inlen, "veth.ipv6.route\n");
		break;
	case LXC_NET_MACVLAN:
		strprint(retv, inlen, "macvlan.mode\n");
		break;
	case LXC_NET_IPVLAN:
		strprint(retv, inlen, "ipvlan.mode\n");
		strprint(retv, inlen, "ipvlan.isolation\n");
		break;
	case LXC_NET_VLAN:
		strprint(retv, inlen, "vlan.id\n");
		break;
	case LXC_NET_PHYS:
		break;
	}

	return fulllen;
}
