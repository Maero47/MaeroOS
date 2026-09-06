user_pref("gfx.webrender.software", true);
user_pref("gfx.webrender.software.opengl", false);
user_pref("layers.acceleration.disabled", true);
user_pref("layers.gpu-process.enabled", false);
user_pref("media.gpu-process-decoder", false);
user_pref("webgl.disabled", true);
user_pref("gfx.x11-glx.disabled", true);
user_pref("dom.ipc.processCount", 1);
user_pref("browser.tabs.remote.autostart", false);
user_pref("security.sandbox.content.level", 0);
user_pref("media.rdd-process.enabled", false);
user_pref("network.process.enabled", false);
/* NB (2026-07-03): dom.ipc.processPrelaunch.enabled=true was TESTED to try to
 * avoid the synchronous content-process launch deadlock (main thread parks in
 * GetNewOrUsedLaunchingBrowserProcess → WaitForProcessHandle) — it did NOT help;
 * the tab-creation path still does a synchronous launch and blocks identically.
 * Kept false (known state).  The deadlock is Firefox-internal (launch completion
 * dispatched to the parked main thread), unbreakable from kernel/config. */
/* RETEST 2026-07-04 (post futex-addrspace-scoping fix): prelaunch=true was ruled
 * out on 2026-07-03 when NO content launch ever completed (cross-process condvar
 * mis-routing). Now the FIRST launch completes but a SECOND GetNewOrUsedBrowser
 * Process → WaitForProcessHandle (condvar aXXXXcd8) hangs with no fork/socketpair.
 * With prelaunch ON, a preallocated process launches early and (now that launches
 * complete) may be READY when the tab asks → no synchronous second wait. */
user_pref("dom.ipc.processPrelaunch.enabled", true);
user_pref("media.cubeb.sandbox", false);
user_pref("accessibility.force_disabled", 1);
user_pref("toolkit.telemetry.enabled", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage", "about:blank");
user_pref("app.update.enabled", false);
/* Suppress startup UI that crash-loops on this minimal GTK (no icon theme):
 * crash-recovery / session-restore prompt, first-run welcome, "what's new". A
 * crash on launch otherwise shows the restore dialog, which itself renders GTK
 * theme icons → NULL pixbuf → crash → relaunch → restore dialog … */
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.sessionstore.max_resumed_crashes", 0);
user_pref("toolkit.startup.max_resumed_crashes", -1);
user_pref("browser.startup.page", 0);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.firstrunSkipsHomepage", true);
user_pref("startup.homepage_welcome_url", "");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("startup.homepage_override_url", "");
user_pref("browser.messaging-system.whatsNewPanel.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("browser.disableResetPrompt", true);
user_pref("browser.rights.3.shown", true);
user_pref("browser.startup.couldRestoreSession.count", 0);
/* Use the native (server-side, maeroX-drawn) titlebar instead of Firefox's CSD
 * titlebar — avoids GTK loading the built-in symbolic window-control icons that
 * fail to load here and crash before paint (paired with GTK_CSD=0 in ff.c). */
user_pref("browser.tabs.inTitlebar", 0);
/* TRUE: Firefox draws its OWN (non-native, bundled) titlebar buttons instead of
 * loading GTK's native window-minimize/maximize/close-symbolic theme icons —
 * which fail to decode from libgtk's gresource here → NULL pixbuf → crash.
 * (Had this backwards as false, which selects the native GTK-themed icons and
 * triggers the crash.)  True = use Firefox's own button graphics, no GTK
 * symbolic-icon load. */
user_pref("widget.gtk.non-native-titlebar-buttons.enabled", true);
/* The startup wedge is a chrome-JS modal window opened via
 * nsWindowWatcher::OpenWindowInternal → AppWindow::ShowModal BEFORE the browser
 * window (proven by symbolized kernel backtrace).  On a fresh profile this is
 * the first-run migration/import wizard and/or the mstone-upgrade path.  With no
 * input and no first paint the modal never dismisses → main thread spins in
 * ShowModal forever → browser.xhtml never opens.  Suppress every first-run/
 * migration/upgrade modal so Firefox goes straight to the browser window. */
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.migrate.automigrate.enabled", false);
user_pref("browser.migrate.interactions.bookmarks.enabled", false);
user_pref("browser.migrate.preferences.enabled", false);
user_pref("browser.migrate.showBookmarksToolbarAfterMigration", false);
user_pref("browser.migrate.content-modal.enabled", false);
user_pref("browser.disableResetPrompt", true);
user_pref("browser.startup.upgradeDialog.enabled", false);
user_pref("browser.tabs.warnOnClose", false);
user_pref("browser.tabs.warnOnCloseOtherTabs", false);
user_pref("browser.warnOnQuit", false);
user_pref("app.normandy.first_run", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
user_pref("trailhead.firstrun.didSeeAboutWelcome", true);
user_pref("browser.contentblocking.introCount", 20);
user_pref("doh-rollout.doneFirstRun", true);
/* Prompts: force any content/tab modal prompts to be non-blocking (never open a
 * separate modal window that would spin AppWindow::ShowModal). */
user_pref("prompts.tab_modal.enabled", true);

/* 2026-07-05: the chrome document (browser.xhtml) stalls during AddonManager /
 * extension-system startup (extensionProcessScriptLoader.js, AddonManager.sys.mjs,
 * EnterprisePolicies) → AppWindow::Show → nsWindow::Show never fires → window
 * never maps → no paint.  Disable the heavy/async addon+policy+telemetry startup
 * so the chrome document can finish loading and show the window. */
user_pref("extensions.startupScanScopes", 0);
user_pref("extensions.autoDisableScopes", 15);
user_pref("extensions.enabledScopes", 0);
user_pref("extensions.getAddons.cache.enabled", false);
user_pref("extensions.update.enabled", false);
user_pref("extensions.systemAddon.update.enabled", false);
user_pref("extensions.webextensions.background-delayed-startup", true);
user_pref("browser.policies.enabled", false);
user_pref("toolkit.telemetry.unified", false);
user_pref("toolkit.telemetry.server", "");
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("app.normandy.enabled", false);
user_pref("browser.contentblocking.database.enabled", false);
user_pref("network.trr.mode", 5);
