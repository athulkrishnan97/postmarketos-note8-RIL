// greatlte (Mali-G71 MP20, Panfrost): Firefox keeps WebRender on the CPU
// with this driver by default. Force the GPU renderer (forcing dmabuf
// surfaces as well gave rendering errors).
// Copy into the Firefox profile directory
// (~/.config/mozilla/firefox/<profile>/user.js) and restart Firefox.
user_pref("gfx.webrender.all", true);
user_pref("gfx.webrender.software", false);
user_pref("layers.acceleration.force-enabled", true);
