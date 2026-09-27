// Web Serial side of Serial Tool (WASM). Loaded with --pre-js, so it shares the
// module scope with the EM_JS wrappers in serial.cpp. Everything here is async;
// the C++ side never waits: it polls ST each frame for RX bytes, messages,
// port state and control-line lamps.
var ST = {
  port: null, reader: null, writer: null, keepReading: false,
  state: 0,              // 0 closed, 1 opening, 2 open, 3 error
  signals: 0,            // bit0 CTS, bit1 DSR, bit2 DCD, bit3 RI
  signalTimer: null,
  rx: [], rxHead: 0,     // queued Uint8Array chunks + read offset into rx[0]
  msgs: [],              // [kind, text]  kind: 1 log line, 2 status ok, 3 status err, 4 status info
  writeChain: Promise.resolve(),
  file: null, fileName: '',

  supported() { return typeof navigator !== 'undefined' && 'serial' in navigator; },
  msg(kind, text) { this.msgs.push([kind, text]); },

  async connect(opts) {
    if (!this.supported() || this.state === 1 || this.state === 2) return;
    this.state = 1;
    try {
      this.port = await navigator.serial.requestPort();
    } catch (e) {
      this.state = 0; this.port = null;
      this.msg(4, 'Port selection cancelled.');
      return;
    }
    try {
      await this.port.open(opts.open);
    } catch (e) {
      this.state = 3; this.port = null;
      this.msg(3, 'Could not open port: ' + e.message);
      return;
    }
    try { await this.port.setSignals({ dataTerminalReady: opts.dtr, requestToSend: opts.rts }); } catch {}
    this.writer = this.port.writable.getWriter();
    this.writeChain = Promise.resolve();
    this.keepReading = true;
    this.state = 2;
    const o = opts.open;
    this.msg(1, '-- Port opened @ ' + o.baudRate + ' baud, ' + o.dataBits +
                o.parity[0].toUpperCase() + o.stopBits + ' --');
    this.msg(2, 'Connected.');
    this.readLoop();
    this.signalTimer = setInterval(() => this.pollSignals(), 250);
  },

  async readLoop() {
    const port = this.port;
    while (port && port === this.port && port.readable && this.keepReading) {
      this.reader = port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value && value.length) this.rx.push(value);
        }
      } catch (e) {
        if (this.keepReading) this.msg(1, '-- Read error: ' + e.message + ' --');
      } finally {
        try { this.reader.releaseLock(); } catch {}
        this.reader = null;
      }
    }
  },

  async pollSignals() {
    if (!this.port) return;
    try {
      const s = await this.port.getSignals();
      this.signals = (s.clearToSend ? 1 : 0) | (s.dataSetReady ? 2 : 0) |
                     (s.dataCarrierDetect ? 4 : 0) | (s.ringIndicator ? 8 : 0);
    } catch {}
  },

  async disconnect(reason) {
    if (!this.port) return;
    const port = this.port;
    this.keepReading = false;
    if (this.signalTimer) { clearInterval(this.signalTimer); this.signalTimer = null; }
    try { if (this.reader) await this.reader.cancel(); } catch {}
    try { await this.writeChain; } catch {}
    try { if (this.writer) await this.writer.close(); } catch {}
    try { if (this.writer) this.writer.releaseLock(); } catch {}
    this.writer = null;
    try { await port.close(); } catch {}
    this.port = null;
    this.state = 0;
    this.signals = 0;
    if (reason) this.msg(1, reason);
    this.msg(1, '-- Port closed --');
  },

  // Writes are chained so they reach the port in the order C++ issued them.
  write(bytes) {
    const w = this.writer;
    if (!w) return;
    this.writeChain = this.writeChain
      .then(() => w.write(bytes))
      .catch(e => this.msg(3, 'Send failed: ' + e.message));
  },

  setSignals(sig) { if (this.port) this.port.setSignals(sig).catch(() => {}); },

  async sendBreak() {
    if (!this.port) return;
    const port = this.port;
    try {
      await port.setSignals({ break: true });
      setTimeout(() => port.setSignals({ break: false }).catch(() => {}), 250);
      this.msg(2, 'Break sent.');
    } catch (e) { this.msg(3, 'Break failed: ' + e.message); }
  },

  // "Send file…": the picked file is parked here until C++ takes it, so the
  // bytes go through the same path (counters, echo) as typed input.
  pickFile() {
    const input = document.createElement('input');
    input.type = 'file';
    input.onchange = async () => {
      const f = input.files && input.files[0];
      if (!f) return;
      this.file = new Uint8Array(await f.arrayBuffer());
      this.fileName = f.name;
    };
    input.click();
  },

  download(name, bytes) {
    const blob = new Blob([bytes], { type: 'text/plain' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  },
};

if (ST.supported()) {
  navigator.serial.addEventListener('disconnect', ev => {
    if (ST.port && ev.target === ST.port) ST.disconnect('-- Device disconnected --');
  });
}
