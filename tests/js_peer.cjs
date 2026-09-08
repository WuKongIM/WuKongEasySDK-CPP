// Runs the real, separately built reference JS SDK; no wire emulation.
const { WKIM, WKIMEvent, WKIMChannelType, WKIMDeviceFlag } = require(process.env.WKIM_JS_ENTRY);
const im = WKIM.init(process.env.WKIM_URL, {
  uid: 'cpp-bob', token: process.env.WKIM_BOB_TOKEN,
  deviceId: 'js-bob-device', deviceFlag: WKIMDeviceFlag.Desktop,
}, { singleton: false, debugLogging: false });
im.on(WKIMEvent.Message, (message) => {
  im.send(message.fromUid, WKIMChannelType.Person, message.payload).catch(() => {
    process.stderr.write('JS send failed\n'); process.exitCode = 1; im.destroy();
  });
});
im.connect().then(() => process.stdout.write('READY\n')).catch(() => {
  process.stderr.write('JS connect failed\n'); process.exitCode = 1; im.destroy();
});
process.on('SIGTERM', () => { im.destroy(); process.exit(); });
