/* SPDX-License-Identifier: GPL-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "params.h"
#include "logging.h"
#include "util.h"

#define PROG_NAME "xdp-loader"


static const struct loadopt {
	bool help;
	struct iface iface;
	char *filename;
	char *pin_path;
	char *section_name;
	bool force;
	bool skb_mode;
} defaults_load = {
};

static struct prog_option load_options[] = {
	DEFINE_OPTION("force", OPT_BOOL, struct loadopt, force,
		      .short_opt = 'F',
		      .help = "Force loading of XDP program"),
	DEFINE_OPTION("skb-mode", OPT_BOOL, struct loadopt, skb_mode,
		      .short_opt = 's',
		      .help = "Load XDP program in SKB (generic) mode"),
	DEFINE_OPTION("pin-path", OPT_STRING, struct loadopt, pin_path,
		      .short_opt = 'p',
		      .help = "Path to pin maps under (must be in bpffs)."),
	DEFINE_OPTION("section-name", OPT_STRING, struct loadopt, section_name,
		      .short_opt = 's',
		      .help = "ELF section name of program to load (default: first in file)."),
	DEFINE_OPTION("dev", OPT_IFNAME, struct loadopt, iface,
		      .positional = true,
		      .metavar = "<ifname>",
		      .required = true,
		      .help = "Load on device <ifname>"),
	DEFINE_OPTION("filename", OPT_STRING, struct loadopt, filename,
		      .positional = true,
		      .metavar = "<filename>",
		      .required = true,
		      .help = "Load program from <progfile>"),
	END_OPTIONS
};

int do_load(const void *cfg, const char *pin_root_path)
{
	const struct loadopt *opt = cfg;
	struct bpf_object *obj = NULL;
	char errmsg[STRERR_BUFSIZE];
	int err = EXIT_SUCCESS;
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts,
			    .pin_root_path = opt->pin_path);

	pr_debug("Loading file '%s' on interface '%s'.\n",
		 opt->filename, opt->iface.ifname);

	/* libbpf spits out a lot of unhelpful error messages while loading.
	 * Silence the logging so we can provide our own messages instead; this
	 * is a noop if verbose logging is enabled.
	 */
	silence_libbpf_logging();

retry:

	obj = open_bpf_file(opt->filename, &opts);
	err = libbpf_get_error(obj);
	if (err) {
		pr_warn("Couldn't load BPF program: %s\n", strerror(-err));
		obj = NULL;
		goto out;
	}

	if (!opt->pin_path) {
		struct bpf_map *map;

		bpf_object__for_each_map(map, obj)
			bpf_map__set_pin_path(map, NULL);
	}

	err = bpf_object__load(obj);
	if (err) {
		if (err == -EPERM) {
			pr_debug("Permission denied when loading eBPF object; "
				 "raising rlimit and retrying\n");

			if (!double_rlimit()) {
				bpf_object__close(obj);
				goto retry;
			}
		}

		libbpf_strerror(err, errmsg, sizeof(errmsg));
		pr_warn("Couldn't load eBPF object: %s(%d)\n", errmsg, err);
		goto out;
	}

	err = attach_xdp_program(obj, opt->section_name, &opt->iface, opt->force,
				 opt->skb_mode, opt->pin_path);
	if (err) {
		pr_warn("Couldn't attach XDP program on iface '%s'\n",
			opt->iface.ifname);
		goto out;
	}

out:
	if (obj)
		bpf_object__close(obj);
	return err;
}

static int remove_iface_program(const struct iface *iface,
				const struct bpf_prog_info *info,
				bool is_skb, void *arg)
{
	char *pin_root_path = arg;
	int err;

	err = detach_xdp_program(iface, pin_root_path);
	if (err) {
		pr_warn("Removing XDP program on iface %s failed (%d): %s\n",
			iface->ifname, -err, strerror(-err));
	}
	return err;
}


static const struct unloadopt {
	bool all;
	struct iface iface;
} defaults_unload = {};

static struct prog_option unload_options[] = {
	DEFINE_OPTION("dev", OPT_IFNAME, struct unloadopt, iface,
		      .positional = true,
		      .metavar = "<ifname>",
		      .help = "Unload from device <ifname>"),
	DEFINE_OPTION("all", OPT_BOOL, struct unloadopt, all,
		      .short_opt = 'a',
		      .help = "Unload from all interfaces"),
	END_OPTIONS
};

int do_unload(const void *cfg, const char *pin_root_path)
{
	const struct unloadopt *opt = cfg;
	struct bpf_prog_info info;
	int err = EXIT_SUCCESS;
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts,
			    .pin_root_path = pin_root_path);

	if (opt->all) {
		pr_debug("Removing XDP programs from all interfaces\n");
		err = iterate_iface_programs_all(pin_root_path, remove_iface_program,
						 (void *)pin_root_path);
		goto out;
	}

	if (!opt->iface.ifindex) {
		pr_warn("Must specify ifname or --all\n");
		err = EXIT_FAILURE;
		goto out;
	}

	err = get_loaded_program(&opt->iface, NULL, &info);
	if (err) {
		pr_warn("No XDP program loaded on %s\n", opt->iface.ifname);
		err = EXIT_FAILURE;
		goto out;
	}

	err = remove_iface_program(&opt->iface, &info, false, (void *)pin_root_path);
	if (err)
		goto out;

out:
	return err;
}

static struct prog_option status_options[] = {
	END_OPTIONS
};

int print_iface_status(const struct iface *iface, const struct bpf_prog_info *info,
		       bool is_skb, void *arg)
{
	char tag[BPF_TAG_SIZE*2+1];
	char namebuf[100];
	int i;

	for (i = 0; i < BPF_TAG_SIZE; i++) {
		sprintf(&tag[i*2], "%02x", info->tag[i]);
	}
	tag[BPF_TAG_SIZE*2] = '\0';

	if (is_skb)
		snprintf(namebuf, sizeof(namebuf), "%s (skb mode)", iface->ifname);
	else
		snprintf(namebuf, sizeof(namebuf), "%s", iface->ifname);
	printf("%-16s %-16s %-8s %-4d %s\n",
	       iface->ifname,
	       info->name,
	       is_skb ? "skb" : "native",
	       info->id, tag);
	return 0;
}

int do_status(const void *cfg, const char *pin_root_path)
{
	int err = EXIT_SUCCESS;

	printf("CURRENT XDP PROGRAM STATUS:\n\n");
	printf("%-16s %-16s Mode     ID   tag\n", "Interface", "Program name");
	printf("----------------------------------------------------------------\n");

	err = iterate_iface_programs_all(pin_root_path, print_iface_status, NULL);
	printf("\n");

	return err;
}

int do_help(const void *cfg, const char *pin_root_path)
{
	fprintf(stderr,
		"Usage: xdp-loader COMMAND [options]\n"
		"\n"
		"COMMAND can be one of:\n"
		"       load        - load an XDP program on an interface\n"
		"       unload      - unload an XDP program from an interface\n"
		"       status      - show current XDP program status\n"
		"       help        - show this help message\n"
		"\n"
		"Use 'xdp-loader COMMAND --help' to see options for each command\n");
	return -1;
}


static const struct prog_command cmds[] = {
	DEFINE_COMMAND(load, "Load an XDP program on an interface"),
	DEFINE_COMMAND(unload, "Unload an XDP program from an interface"),
	DEFINE_COMMAND_NODEF(status, "Show XDP program status"),
	{.name = "help", .func = do_help, .no_cfg = true},
	END_COMMANDS
};

union all_opts {
	struct loadopt load;
	struct unloadopt unload;
};

int main(int argc, char **argv)
{
	if (argc > 1)
		return dispatch_commands(argv[1], argc-1, argv+1,
					 cmds, sizeof(union all_opts),
					 PROG_NAME);

	return do_help(NULL, NULL);
}