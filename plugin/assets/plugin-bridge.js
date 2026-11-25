// plugin-bridge.js
// Injected right after webrack.js inside the native CLAP plugin's WebView.
// Replaces WebAudio/AudioWorklet/WebMIDI with a lock-free IPC bridge to the C++ RackEngine.

(function () {
	if (typeof WR === "undefined" || !WR.Context) return;
	if (typeof window.gasfreePost !== "function") return; // running in a normal browser

	window.GASFREE_NATIVE = true;

	var nativeSR = 48000;
	var nativeFrame = 0;
	var nativeTypes = {};
	var activeRack = null;
	var initializedFromHost = false;

	function sendToNative(obj) {
		try {
			window.gasfreePost(JSON.stringify(obj));
		} catch (err) {
			console.error("gasfreePost failed:", err);
		}
	}

	var proto = WR.Context.prototype;

	proto.init = function () {
		activeRack = this;
		if (!this.context) {
			this.context = {
				state: "running",
				get sampleRate() { return nativeSR; },
				get currentTime() { return nativeFrame / nativeSR; },
				suspend: function () { return Promise.resolve(); },
				resume: function () { return Promise.resolve(); }
			};
		}
		if (!this.node) {
			this.node = {
				port: {
					postMessage: function (msg) { sendToNative(msg); }
				}
			};
		}
		this.midi.status = "DAW MIDI";
		this.io_devices = {
			outputs: [{ id: "", label: "DAW MAIN OUT" }],
			inputs: [{ id: "", label: "DAW AUDIO IN" }],
			status: "DAW AUDIO"
		};
		this._sent = null;
		this.updateIO();
		this.sendScale();
		this.sync();
		this._initializing = Promise.resolve();
		return this._initializing;
	};

	proto.post = function (msg) {
		if (!msg) return;
		sendToNative(msg);
	};

	proto.sync = function () {
		activeRack = this;
		var json = JSON.stringify(this.settings);
		if (json === this._sent) return;
		this._sent = json;
		sendToNative({
			type: "settings",
			settings: WR.flatten(this.settings),
			rawSettings: this.settings,
			tempo: this.tempo()
		});
	};

	proto.updateNative = function () {
		// Convolution reverb runs natively in C++ DSP (zero WebAudio slots needed)
	};

	proto.updateIO = function () {
		var key = this.io.master + "/" + this.io.input_gain;
		if (key === this._io_sent) return;
		this._io_sent = key;
		sendToNative({
			type: "io",
			master: this.io.master,
			input_gain: this.io.input_gain
		});
	};

	proto.updateMeter = function () {
		return this.meter;
	};

	proto.initMIDI = function () {
		this.midi.status = "DAW MIDI";
		this.notifyMIDI();
		return Promise.resolve(true);
	};

	proto.midiInputName = function () {
		return "DAW MIDI IN";
	};

	proto.canSelectOutput = function () { return false; };
	proto.refreshDevices = function () { return Promise.resolve(); };
	proto.setInput = function () { return Promise.resolve(); };

	// Receive telemetry & host state updates from C++
	window.__gasfreeOnNative = function (msg) {
		if (!msg || typeof msg !== "object") return;
		var r = activeRack || (typeof MAIN_RACK !== "undefined" ? MAIN_RACK : null);
		if (!r) return;

		if (msg.type === "state") {
			if (msg.sampleRate > 0) nativeSR = msg.sampleRate;
			if (typeof msg.currentFrame === "number") nativeFrame = msg.currentFrame;
			if (msg.modules) r.onState(msg.modules);
			if (typeof msg.load === "number") {
				r.cpu = r.cpu == null ? msg.load : r.cpu * 0.5 + msg.load * 0.5;
				r.cpu_timer = "performance";
			}
			if (Array.isArray(msg.meter) && msg.meter.length >= 2) {
				r.meter[0] = msg.meter[0];
				r.meter[1] = msg.meter[1];
			}
		} else if (msg.type === "init") {
			if (msg.sampleRate > 0) nativeSR = msg.sampleRate;
			if (Array.isArray(msg.nativeTypes)) {
				nativeTypes = {};
				msg.nativeTypes.forEach(function (t) { nativeTypes[t] = true; });
			}
			if (typeof msg.master === "number") {
				r.io.master = msg.master;
			}
			if (msg.hasState && msg.settings && typeof msg.settings === "object") {
				r.setSettings(msg.settings);
				r._sent = JSON.stringify(r.settings);
				if (typeof updatePanelHeight === "function") updatePanelHeight();
			} else if (!initializedFromHost) {
				// Push the UI's initial rack back to C++ so both sides match
				r._sent = null;
				r.sync();
			}
			initializedFromHost = true;
		}
	};

	// Once code.js has created `rack` and wired DOM elements, boot the native bridge
	window.addEventListener("load", function () {
		var r = typeof MAIN_RACK !== "undefined" ? MAIN_RACK : (typeof rack !== "undefined" ? rack : null);
		if (r) {
			activeRack = r;
			r.init();
		}
		var pwr = document.getElementById("power");
		if (pwr) {
			pwr.textContent = "Panic";
			pwr.title = "Release all held notes and stop internal sequencers (DAW owns audio)";
			pwr.classList.add("on");
			// Clone to replace code.js click listener with Panic action
			var clone = pwr.cloneNode(true);
			pwr.parentNode.replaceChild(clone, pwr);
			clone.addEventListener("click", function () {
				if (activeRack) {
					activeRack.allNotesOff();
					activeRack.transport("play", false);
				}
			});
		}
		var status = document.getElementById("status");
		if (status && !status.textContent) {
			status.textContent = "CLAP Native Engine";
		}
		if (typeof window.gasfreeReady === "function") {
			window.gasfreeReady();
		}
	});
})();
