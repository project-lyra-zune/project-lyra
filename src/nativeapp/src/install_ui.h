#pragma once

// Each stage has a baked label status_<slug>.bgra; keep in sync with STAGES in
// content/installer/bake-installer-assets.py.
enum InstallStage {
	STAGE_PREPARE = 0,
	STAGE_LOADER,
	STAGE_DAEMON,
	STAGE_MODS,
	STAGE_DONE,
	STAGE_COUNT
};

// Order matches kUninstallLabelFiles and the baked status_<slug>.bgra pair.
enum UninstallStage {
	UNINSTALL_WORKING = 0,
	UNINSTALL_REBOOT
};

// Full-screen GL install splash: runs `work` (the payload copy) on a worker
// thread that calls set_install_stage as it goes, animates the wash until the
// copy finishes, then reboots. If GL bring-up fails
// the copy still completes and the result is reported modally.
void run_install_ui(void (*work)(void));

// Uninstall variant: same splash, its own two-label set (UninstallStage). `work` arms
// the boot-time wipe by writing the pending marker; the splash then reboots.
void run_uninstall_ui(void (*work)(void));

// Publish the current stage index from the worker; the render loop pulls it each frame.
// The index selects into the active label set (the install stages, or the uninstall pair).
void set_install_stage(int stage);
