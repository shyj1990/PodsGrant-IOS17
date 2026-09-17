// PodsGrant Tweak.c —— 带完整调试日志版（用于定位 iOS17 上 custom product id mapping 不生效）
// 用法：用本文件整体替换 fork 仓库根目录的 Tweak.c，其余文件不动，然后跑 GitHub Actions 构建。
// 日志会同时写到 /tmp/PodsGrant.log 和 /var/mobile/Library/PodsGrant.log（用 Filza 查看）。
//
// 与原版差异：仅新增日志，不改动任何 hook 逻辑。
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <substrate.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include "os_log_handler.h"
#include "general.h"

static unsigned int product_id_offset;
FILE *log_file;
static struct podsgrant_settings *settings;
extern unsigned char PGS_global_os_ver;

// ---------------- 日志工具 ----------------
#define PGS_LOG_PATH_1 "/tmp/PodsGrant.log"
#define PGS_LOG_PATH_2 "/var/mobile/Library/PodsGrant.log"

static void pgs_log(const char *fmt, ...) {
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	const char *paths[2] = { PGS_LOG_PATH_1, PGS_LOG_PATH_2 };
	for (int i = 0; i < 2; i++) {
		FILE *f = fopen(paths[i], "a");
		if (f) {
			fprintf(f, "%s\n", msg);
			fflush(f);
			fclose(f);
		}
	}
}

unsigned int (*orig_1002E1F9C)(void *a1, void *a2, void *a3, void *a4, void *a5);
unsigned int my_1002E1F9C(void *a1, void *a2, void *a3, void *a4, void *a5) {
	static int pidlog_cap = 0;
	uint32_t raw = *(uint32_t *)((char *)a1 + product_id_offset);
	uint16_t patched = PGS_patchProductId(settings, raw);
	if (patched) {
		*(uint32_t *)((char *)a1 + product_id_offset) = (uint32_t)patched;
	}
	if (pidlog_cap < 300) {
		pgs_log("[hook:pidfunc] raw=%u patched=%u final=%u (offset=%u)",
				raw, patched, *(uint32_t *)((char *)a1 + product_id_offset), product_id_offset);
		pidlog_cap++;
	}
	return orig_1002E1F9C(a1, a2, a3, a4, a5);
}

unsigned int (*abilityFuncOrig)(void *, unsigned int abilityID);
unsigned int abilityFunc(void *a1, unsigned int abilityID) {
	uint16_t patched = PGS_patchProductId(settings, *(uint32_t *)((char *)a1 + product_id_offset));
	if (patched) {
		*(uint32_t *)((char *)a1 + product_id_offset) = (uint32_t)patched;
	}
	if (*(unsigned int *)((char *)a1 + product_id_offset) == 0x200E) {
		if (abilityID == 12 || abilityID == 26) {
			return 1;
		}
	}
	return abilityFuncOrig(a1, abilityID);
}

void *(*supportRemoteVolumeChangeOriginal)(void *, BOOL);
void *supportRemoteVolumeChange(void *a1, BOOL support) {
	return supportRemoteVolumeChangeOriginal(a1, 1);
}

void *(*supportSoftwareVolumeOriginal)(void *, BOOL);
void *supportSoftwareVolume(void *a1, BOOL support) {
	return supportSoftwareVolumeOriginal(a1, 1);
}

void *(*recvLoggingHandlerOriginal)(void *a1, void *a2, void *a3, void *a4, char *a5);
void *recvLoggingHandler(void *a1, void *a2, void *a3, void *a4, char *a5) {
	return NULL;
}

void *orig_os_log_impl;
void my_os_log_impl(void *dso, void *log_ent, uint64_t type, const char *format, uint8_t *buf, uint32_t size) {
	format_os_log(log_file, log_ent, format, buf, size, 0);
	fflush(log_file);
}

__attribute__((destructor))
static void __podsgrant_main_teardown(void) {
	if (settings) {
		PGS_freeSettings(settings);
	}
}

__attribute__((constructor))
static void __podsgrant_main_construct(void) {
	pgs_log("================================================");
	pgs_log("=== PodsGrant constructor entered, pid=%d ===", getpid());
	{
		char exec_path[512] = {0};
		uint32_t len = 512;
		_NSGetExecutablePath(exec_path, &len);
		pgs_log("exec_path=%s", exec_path);
		if (memcmp(exec_path, "/usr/sbin/bluetoothd", 21) != 0) {
			pgs_log("NOT bluetoothd -> return (dylib injected into wrong process)");
			settings = NULL;
			return;
		}
		pgs_log("target confirmed: bluetoothd");
	}
	settings = PGS_readSettings(0);
	pgs_log("settings: enabled=%d custom_map_cnt=%d addr_map_cnt=%d",
			settings->is_tweak_enabled, settings->product_id_mapping_cnt, settings->address_mapping_cnt);
	if (settings->product_id_mapping_cnt) {
		for (int i = 0; i < settings->product_id_mapping_cnt; i++) {
			pgs_log("  custom map[%d]: %u -> %u",
					i, settings->product_id_mapping[i].original, settings->product_id_mapping[i].target);
		}
	} else {
		pgs_log("  WARNING: no custom product id mapping entries in settings file!");
	}
	if (!settings->is_tweak_enabled) {
		pgs_log("tweak disabled in settings -> return");
		PGS_freeSettings(settings);
		settings = NULL;
		return;
	}
	char os_ver_buf[12];
	size_t os_ver_len = 12;
	int sysctl_result = sysctlbyname("kern.osproductversion", os_ver_buf, &os_ver_len, NULL, 0);
	if (sysctl_result != 0) {
		pgs_log("FAILED to get OS version (sysctl)");
		FILE *err_file = fopen("/tmp/bluetoothd.err.log", "a");
		if (err_file) {
			fprintf(err_file, "bluetoothd [PodsGrant]: Failed to get OS version.\n");
			fclose(err_file);
		}
		abort();
	}
	os_ver_buf[os_ver_len] = 0;
	for (char *ptr = os_ver_buf; *ptr != 0; ptr++) {
		if (*ptr == '.') {
			*ptr = 0;
			PGS_global_os_ver = atoi(os_ver_buf);
		}
	}
	pgs_log("os_version parsed -> PGS_global_os_ver=%d", PGS_global_os_ver);

	uint64_t all_addr[3] = {0, 0, 0};
	int found = PGS_findAddresses(all_addr, &product_id_offset);
	pgs_log("PGS_findAddresses -> found=%d addr0=%p addr1=%p addr2=%p product_id_offset=%u",
			found, (void *)all_addr[0], (void *)all_addr[1], (void *)all_addr[2], product_id_offset);
	if (!found) {
		pgs_log("PGS_findAddresses FAILED -> return (no hooks installed, silent in original build)");
		return;
	}

	uint64_t bin_vmaddr_slide = 0;
#ifndef IS_ROOTLESS
	bin_vmaddr_slide = _dyld_get_image_vmaddr_slide(0);
#else
	int image_count = _dyld_image_count();
	for (int i = 0; i < image_count; i++) {
		const char *img_name = _dyld_get_image_name(i);
		if (memcmp(img_name, "/usr/sbin/bluetoothd\0", 21) == 0) {
			bin_vmaddr_slide = _dyld_get_image_vmaddr_slide(i);
			break;
		}
	}
	if (!bin_vmaddr_slide) {
		pgs_log("FAILED: image index for bluetoothd not found (rootless slide lookup)");
		FILE *err_file = fopen("/tmp/bluetoothd.err.log", "a");
		if (err_file) {
			fprintf(err_file, "bluetoothd [PodsGrant]: image index for `bluetoothd` not found.\n");
			fclose(err_file);
		}
		abort();
	}
#endif
	pgs_log("bin_vmaddr_slide=%p | hook0=%p hook1=%p hook2=%p",
			(void *)bin_vmaddr_slide,
			(void *)(bin_vmaddr_slide + all_addr[0]),
			(void *)(bin_vmaddr_slide + all_addr[1]),
			(void *)(bin_vmaddr_slide + all_addr[2]));

	MSHookFunction((void *)(bin_vmaddr_slide + all_addr[0]), (void *)&my_1002E1F9C, (void **)&orig_1002E1F9C);
	pgs_log("hook0 (product id func) installed OK");
	MSHookFunction((void *)(bin_vmaddr_slide + all_addr[1]), (void *)&abilityFunc, (void **)&abilityFuncOrig);
	pgs_log("hook1 (ability func) installed OK");
	if (all_addr[2]) {
		MSHookFunction((void *)(bin_vmaddr_slide + all_addr[2]), (void *)&supportRemoteVolumeChange, (void **)&supportRemoteVolumeChangeOriginal);
		pgs_log("hook2 (remote volume func) installed OK");
	} else {
		pgs_log("hook2 skipped (address not found)");
	}
	pgs_log("=== constructor done, hooks active ===");
}
