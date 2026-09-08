// The pinned JS SDK owns all protocol behavior; this file only bridges test commands.
const readline = require('node:readline');
const { WKIM, WKIMDeviceFlag, WKIMEvent } = require(process.env.WKIM_JS_ENTRY);
const emit = value => process.stdout.write(JSON.stringify(value) + '\n');
const im = WKIM.init(process.env.WKIM_URL, {
  uid: process.env.WKIM_UID, token: process.env.WKIM_TOKEN,
  deviceFlag: WKIMDeviceFlag.Desktop,
}, { singleton: false, debugLogging: false });
im.on(WKIMEvent.Connect, result => emit({kind: 'connect', result}));
im.on(WKIMEvent.Disconnect, () => emit({kind: 'disconnect'}));
im.on(WKIMEvent.Error, () => emit({kind: 'error'}));
im.on(WKIMEvent.Message, message => emit({kind: 'message', message}));
readline.createInterface({input: process.stdin}).on('line', async line => {
  const command = JSON.parse(line);
  try {
    if (command.kind === 'send') {
      const ack = await im.send(command.uid, 1, command.payload, {clientMsgNo: command.clientMsgNo});
      emit({kind: 'reply', id: command.id, ok: true, ack});
    } else if (command.kind === 'stop') {
      im.destroy();
      emit({kind: 'reply', id: command.id, ok: true, destroyed: true});
      process.exit(0);
    } else { throw new Error('Unknown command'); }
  } catch (error) {
    // Keep only numeric codes and fixed categories; free-form protocol text may contain secrets.
    const code = Number.isInteger(error?.code) ? error.code : null;
    const category = typeof error?.message === 'string' && error.message.startsWith('Request timeout for method ')
      ? 'timeout' : ['Not connected. Call connect() first.', 'WebSocket is not open.', 'Connection closed'].includes(error?.message)
      ? 'connection' : 'request';
    emit({kind: 'reply', id: command.id, ok: false, code, category});
  }
});
im.connect().then(() => emit({kind: 'ready'})).catch(() => {
  emit({kind: 'fatal'}); im.destroy(); process.exit(1);
});
process.on('SIGTERM', () => { im.destroy(); process.exit(0); });
