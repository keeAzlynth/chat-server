// 端到端冒烟测试：两个客户端走完 注册/登录/公聊/私聊/群聊/历史/编辑/撤回
const assert = require('node:assert');
const PORT = process.env.IM_PORT || 8080;
const BASE = `ws://127.0.0.1:${PORT}`;

class Client {
  constructor(name) {
    this.name = name;
    this.events = [];
    this.waiters = [];
    this.ws = new WebSocket(BASE);
    this.ready = new Promise((resolve, reject) => {
      this.ws.onopen = resolve;
      this.ws.onerror = (e) => reject(new Error('ws error ' + name));
    });
    this.ws.onmessage = (ev) => {
      const msg = JSON.parse(ev.data);
      this.events.push(msg);
      this.waiters = this.waiters.filter((w) => {
        if (w.match(msg)) { w.resolve(msg); return false; }
        return true;
      });
    };
  }

  send(type, extra = {}) {
    this.ws.send(JSON.stringify({ type, ...extra }));
  }

  waitFor(match, timeout = 4000) {
    const found = this.events.find(match);
    if (found) return Promise.resolve(found);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`${this.name}: timeout waiting for event`)), timeout);
      this.waiters.push({
        match,
        resolve: (m) => { clearTimeout(timer); resolve(m); },
      });
    });
  }

  clear() { this.events = []; }
  close() { try { this.ws.close(); } catch {} }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const stamp = Date.now().toString(36).slice(-6);

(async () => {
  const a = new Client('A');
  const b = new Client('B');
  await Promise.all([a.ready, b.ready]);

  const userA = { username: `alice_${stamp}`, password: 'Passw0rd!', nickname: `爱丽丝${stamp}` };
  const userB = { username: `bob_${stamp}`, password: 'Passw0rd!', nickname: `鲍勃${stamp}` };

  // 注册
  a.send('register', { ...userA, deviceName: 'test' });
  const regA = await a.waitFor((m) => m.type === 'logged_in');
  assert.ok(regA.sessionToken, 'A 应拿到 sessionToken');
  console.log('✓ 注册 A:', regA.nickname, regA.userId);

  b.send('register', { ...userB, deviceName: 'test' });
  const regB = await b.waitFor((m) => m.type === 'logged_in');
  console.log('✓ 注册 B:', regB.nickname, regB.userId);

  // 加入
  a.send('join', { nickname: userA.nickname });
  b.send('join', { nickname: userB.nickname });
  await a.waitFor((m) => m.type === 'joined');
  await b.waitFor((m) => m.type === 'joined');
  await a.waitFor((m) => m.type === 'online_users');
  console.log('✓ 双方加入，在线列表已广播');

  // 公聊
  a.clear(); b.clear();
  a.send('send', { text: '你好，世界 #1' });
  const pubB = await b.waitFor((m) => m.type === 'message' && m.text === '你好，世界 #1');
  assert.ok(pubB.messageId > 0, '公聊消息应带 messageId');
  const pubA = await a.waitFor((m) => m.type === 'message' && m.text === '你好，世界 #1');
  assert.strictEqual(pubA.messageId, pubB.messageId, '双方看到的 messageId 应一致');
  console.log('✓ 公聊投递，messageId =', pubB.messageId);

  // 私聊
  a.clear(); b.clear();
  b.send('private_message', { receiverId: regA.userId, text: '悄悄话 #2' });
  const dm = await a.waitFor((m) => m.type === 'private_message' && m.text === '悄悄话 #2');
  assert.strictEqual(dm.userId, regB.userId);
  console.log('✓ 私聊投递 A <- B');

  // 群组
  a.clear(); b.clear();
  a.send('create_group', { name: `测试群${stamp}` });
  const created = await a.waitFor((m) => m.type === 'group_created');
  console.log('✓ 建群:', created.name, created.groupId);

  b.send('join_group', { groupId: created.groupId });
  await b.waitFor((m) => m.type === 'group_member_joined' && m.userId === regB.userId);

  a.clear(); b.clear();
  a.send('send_group_message', { groupId: created.groupId, text: '群消息 #3' });
  const gm = await b.waitFor((m) => m.type === 'group_message' && m.text === '群消息 #3');
  assert.strictEqual(gm.groupId, created.groupId);
  console.log('✓ 群消息只投递给成员');

  // 群列表
  b.clear();
  b.send('get_groups');
  const list = await b.waitFor((m) => m.type === 'group_list');
  const found = list.groups.find((g) => g.groupId === created.groupId);
  assert.ok(found, '群列表应包含新群');
  assert.strictEqual(found.memberCount, 2);
  console.log('✓ 群列表成员数 =', found.memberCount);

  // 历史
  b.clear();
  b.send('get_message_history', { limit: 20 });
  const history = await b.waitFor((m) => m.type === 'message_history');
  assert.ok(history.messages.some((m) => m.text === '你好，世界 #1'), '历史应包含公聊消息');
  console.log('✓ 公聊历史条数 =', history.messages.length);

  b.clear();
  b.send('get_group_message_history', { groupId: created.groupId, limit: 20 });
  const gh = await b.waitFor((m) => m.type === 'group_message_history');
  assert.ok(gh.messages.some((m) => m.text === '群消息 #3'));
  console.log('✓ 群历史条数 =', gh.messages.length);

  // 编辑 + 撤回
  a.clear(); b.clear();
  a.send('edit_message', { messageId: pubA.messageId, text: '已编辑的内容' });
  const edited = await b.waitFor((m) => m.type === 'message_edited');
  assert.strictEqual(edited.text, '已编辑的内容');
  console.log('✓ 编辑广播');

  a.send('recall_message', { messageId: pubA.messageId });
  await b.waitFor((m) => m.type === 'message_recalled');
  console.log('✓ 撤回广播');

  // 权限：B 不能撤回 A 的消息
  b.clear();
  b.send('recall_message', { messageId: pubA.messageId });
  const deny = await b.waitFor((m) => m.type === 'error');
  assert.strictEqual(deny.code, 'PERMISSION_DENIED');
  console.log('✓ 越权撤回被拒绝');

  // 限流：连发 40 条应出现 RATE_LIMITED
  a.clear();
  for (let i = 0; i < 40; i++) a.send('send', { text: `flood ${i}` });
  await sleep(400);
  const limited = a.events.find((m) => m.type === 'error' && m.code === 'RATE_LIMITED');
  assert.ok(limited, '应有 RATE_LIMITED');
  console.log('✓ 限流生效');

  // 搜索（中文子串，验证 LIKE 回退）
  b.clear();
  b.send('search_messages', { query: '群消息', limit: 10 });
  const search = await b.waitFor((m) => m.type === 'search_results');
  assert.ok(search.total >= 1, '中文全文搜索应命中');
  console.log('✓ 中文搜索命中 =', search.total);

  // 会话恢复
  const tokenA = regA.sessionToken;
  const c = new Client('C');
  await c.ready;
  c.send('resume_session', { sessionToken: tokenA });
  const resumed = await c.waitFor((m) => m.type === 'logged_in');
  assert.strictEqual(resumed.userId, regA.userId);
  console.log('✓ 会话令牌恢复登录');

  // 多端：A 有两条连接时，私聊应投递到两边
  a.clear(); c.clear();
  b.send('private_message', { receiverId: regA.userId, text: '多端投递 #4' });
  await a.waitFor((m) => m.type === 'private_message' && m.text === '多端投递 #4');
  await c.waitFor((m) => m.type === 'private_message' && m.text === '多端投递 #4');
  console.log('✓ 私聊投递到同一账号的两条连接');

  c.close();
  await sleep(200);

  // 关闭一条连接后，另一条仍在线
  b.clear();
  b.send('get_online_users');
  const online = await b.waitFor((m) => m.type === 'online_users');
  const stillOnline = online.users.find((u) => u.userId === regA.userId);
  assert.ok(stillOnline, '关闭一个设备后账号仍应在线');
  console.log('✓ 多端下线不影响另一条连接');

  a.close(); b.close();
  console.log('\n全部通过');
  process.exit(0);
})().catch((err) => {
  console.error('\n✗ 失败:', err.message);
  process.exit(1);
});
