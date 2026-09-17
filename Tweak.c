// PodsGrant Tweak.c —— 调试 + 路径修复版（定位 iOS17 上 custom product id mapping 不生效）
// 用法：用本文件整体替换 fork 仓库根目录的 Tweak.c，其余文件不动，然后跑 GitHub Actions 构建。
// 日志会同时写到 /tmp/PodsGrant.log 和 /var/mobile/Library/PodsGrant.log（用 Filza 查看）。
//
// 与原版差异：仅新增日志与"路径探测/同步"，不改动任何 hook 逻辑。
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
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

// ---------------- 路径诊断 ----------------
// roothide 下 bluetoothd 守护进程的沙箱禁止读取 /var/mobile/Library/（整个目录 EPERM，实测 errno=1）。
// 实测唯一"两边都能访问、且 realpath 指向同一份真实文件"的位置是 /tmp
// （bluetoothd 与设置 App 对 /tmp/com.lns.pogr.bin 都解析到 /private/var/tmp/com.lns.pogr.bin）。
// 共享设置文件 = /tmp/com.lns.pogr.bin（见 general.h 的 PGS_SETTINGS_FILE）。
// ⚠️ 绝对不要对这个"文件路径"调用 mkdir：上一版 pgs_mkdir_p() 把文件建成了目录，
//    导致 fopen(...,"wb") 报 EISDIR(errno=21) → Save 永远失败；fopen(...,"rb") 能开目录但读到垃圾。
static void pgs_diag_and_fix_settings(void) {
	pgs_log("---- settings path diagnosis ----");
	pgs_log("env HOME=%s", getenv("HOME") ? getenv("HOME") : "(null)");
	pgs_log("env ROOT_PATH=%s", getenv("ROOT_PATH") ? getenv("ROOT_PATH") : "(null)");
	const char *canon = PGS_SETTINGS_FILE;
	pgs_log("PGS_SETTINGS_FILE(canon)=%s", canon);

	// 自愈：清掉历史 bug 留下的"同名目录"（/tmp 本身已存在，无需 mkdir）
	struct stat st;
	if (stat(canon, &st) == 0 && S_ISDIR(st.st_mode)) {
		if (rmdir(canon) == 0)
			pgs_log("[FIX] removed stray directory at canon (previous build's mkdir bug)");
		else
			pgs_log("[FIX] canon is a directory, rmdir failed errno=%d", errno);
	}
	if (stat(canon, &st) == 0) {
		char rp[PATH_MAX]; rp[0] = 0;
		realpath(canon, rp);
		pgs_log("settings file EXISTS size=%lld regular=%d realpath=%s",
				(long long)st.st_size, S_ISREG(st.st_mode), rp[0] ? rp : "?");
	} else {
		pgs_log("settings file MISSING(errno=%d) at canon (settings app has not saved yet)", errno);
	}
	pgs_log("---- end settings path diagnosis ----");
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
			pgs_log("NOT bluetoothd (dylib injected into: %s)", exec_path);
			if (strstr(exec_path, "/Applications/Preferences.app/") != NULL) {
				// 设置 App 被注入 —— 只有它能读写镜像文件（bluetoothd 沙箱禁读 /var/mobile/Library）
				pgs_log("-> this is Settings app: restore share file from mirror if needed");
				PGS_restoreFromMirror();
			} else {
				pgs_log("-> other process, nothing to do");
			}
			settings = NULL;
			return;
		}
		pgs_log("target confirmed: bluetoothd");
	}

	// 路径诊断 + 自动同步（在真正读设置之前）
	pgs_diag_and_fix_settings();

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
