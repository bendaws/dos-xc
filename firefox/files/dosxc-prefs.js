/*
 * DOS-XC /firefox/files/dosxc-prefs.js
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

pref("browser.tabs.remote.autostart", true);
pref("fission.autostart", false);
pref("dom.ipc.processCount", 1);
pref("dom.ipc.processPrelaunch.enabled", false);
pref("dom.ipc.forkserver.enable", false);
pref("security.sandbox.content.level", 1);
pref("network.process.enabled", false);
pref("layers.gpu-process.enabled", false);
pref("media.rdd-process.enabled", false);
pref("media.utility-process.enabled", false);
// With the RDD and utility processes off, the content process decodes media itself.
pref("media.allow-audio-non-utility", true);
// Software decoding is often slower than real time here (QEMU, no GPU).
// Show frames as they are decoded instead of dropping every late one, and
// keep sites such as YouTube on VP9, which decodes faster than AV1.
pref("media.ruin-av-sync.enabled", true);
pref("media.mediasource.av1.enabled", false);
pref("extensions.webextensions.remote", false);

pref("gfx.webrender.software", true);

pref("media.cubeb.sandbox", false);
pref("media.resampling.enabled", true);

pref("dom.max_script_run_time", 0);
pref("dom.max_chrome_script_run_time", 0);

pref("browser.shell.checkDefaultBrowser", false);
pref("browser.aboutwelcome.enabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("datareporting.healthreport.uploadEnabled", false);
pref("toolkit.telemetry.enabled", false);
pref("toolkit.telemetry.unified", false);
pref("browser.cache.disk.enable", false);

pref("network.captive-portal-service.enabled", false);
pref("network.connectivity-service.enabled", false);
pref("browser.region.update.enabled", false);
pref("browser.region.network.url", "");
pref("dom.push.enabled", false);
pref("media.gmp-manager.updateEnabled", false);
pref("media.gmp-gmpopenh264.enabled", false);
pref("browser.translations.enable", false);
pref("browser.ml.enable", false);
pref("browser.ml.chat.enabled", false);
pref("extensions.pocket.enabled", false);
pref("browser.discovery.enabled", false);
pref("browser.newtabpage.activity-stream.showSponsored", false);
pref("browser.newtabpage.activity-stream.showSponsoredTopSites", false);
pref("browser.newtabpage.activity-stream.feeds.section.topstories", false);
pref("browser.newtabpage.activity-stream.feeds.snippets", false);
pref("browser.newtabpage.activity-stream.feeds.telemetry", false);
pref("browser.newtabpage.activity-stream.telemetry", false);
pref("browser.urlbar.quicksuggest.enabled", false);
pref("app.normandy.enabled", false);
pref("app.shield.optoutstudies.enabled", false);
pref("extensions.getAddons.cache.enabled", false);
pref("browser.startup.homepage_override.mstone", "ignore");

pref("font.name-list.serif.x-western", "DejaVu Serif");
pref("font.name-list.sans-serif.x-western", "DejaVu Sans");
pref("font.name-list.monospace.x-western", "DejaVu Sans Mono");
pref("font.name-list.serif.ja", "Noto Sans CJK JP");
pref("font.name-list.sans-serif.ja", "Noto Sans CJK JP");
pref("font.name-list.monospace.ja", "Noto Sans CJK JP");
pref("font.name-list.serif.ko", "Noto Sans CJK KR");
pref("font.name-list.sans-serif.ko", "Noto Sans CJK KR");
pref("font.name-list.monospace.ko", "Noto Sans CJK KR");
pref("font.name-list.serif.zh-CN", "Noto Sans CJK SC");
pref("font.name-list.sans-serif.zh-CN", "Noto Sans CJK SC");
pref("font.name-list.monospace.zh-CN", "Noto Sans CJK SC");
pref("font.name-list.serif.zh-TW", "Noto Sans CJK TC");
pref("font.name-list.sans-serif.zh-TW", "Noto Sans CJK TC");
pref("font.name-list.monospace.zh-TW", "Noto Sans CJK TC");
pref("font.name-list.serif.zh-HK", "Noto Sans CJK HK");
pref("font.name-list.sans-serif.zh-HK", "Noto Sans CJK HK");
pref("font.name-list.monospace.zh-HK", "Noto Sans CJK HK");
pref("font.name-list.serif.x-unicode", "DejaVu Serif");
pref("font.name-list.sans-serif.x-unicode", "DejaVu Sans");
pref("font.name-list.monospace.x-unicode", "DejaVu Sans Mono");
