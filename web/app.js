/* ─────────────────────────────────────────────────────────
   IM 聊天室 — 前端逻辑
   纯原生 JS，无构建步骤；协议与 Android 客户端保持一致。
   ───────────────────────────────────────────────────────── */
(() => {
  'use strict';

  const cfg = window.IM_CONFIG || {};
  const WS_PORT = Number(cfg.wsPort || 8080);
  const MAX_LEN = Number(cfg.maxMessageLength || 500);

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  // ── 状态 ────────────────────────────────────────────────

  const state = {
    ws: null,
    connected: false,
    retry: 0,
    retryTimer: null,
    token: localStorage.getItem('im.token') || '',
    me: null,
    active: 'public',
    people: new Map(),      // userId -> { nickname, level, guest, online }
    convos: new Map(),      // key -> { key, kind, id, title, messages, unread, loaded }
    groups: [],
    dmIds: JSON.parse(localStorage.getItem('im.dms') || '[]'),
    typing: new Map(),      // chatKey -> { nickname, at }
    lastTypingSent: 0,
    lastReadSent: 0,
    members: [],
    pinned: [],
  };

  // ── 工具 ────────────────────────────────────────────────

  const esc = (value) => String(value ?? '')
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');

  function hueFor(id) {
    let hash = 0;
    for (let i = 0; i < String(id).length; i++) {
      hash = (hash * 31 + String(id).charCodeAt(i)) % 360;
    }
    return hash;
  }

  function avatarHtml(name, id, extra = '') {
    const initials = String(name || '?').trim().slice(0, 2).toUpperCase();
    const hue = hueFor(id || name || '?');
    return `<div class="avatar ${extra}" style="background:linear-gradient(135deg,hsl(${hue} 62% 52%),hsl(${(hue + 40) % 360} 66% 58%))">${esc(initials)}</div>`;
  }

  const pad2 = (n) => String(n).padStart(2, '0');

  function fmtTime(ts) {
    const d = new Date(ts * 1000);
    return `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }

  function dayKey(ts) {
    const d = new Date(ts * 1000);
    return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
  }

  function fmtDay(ts) {
    const d = new Date(ts * 1000);
    const today = new Date();
    const yesterday = new Date(Date.now() - 86400000);
    if (dayKey(ts) === dayKey(today.getTime() / 1000)) return '今天';
    if (dayKey(ts) === dayKey(yesterday.getTime() / 1000)) return '昨天';
    return `${d.getMonth() + 1} 月 ${d.getDate()} 日`;
  }

  function fmtBytes(bytes) {
    if (!bytes) return '';
    const units = ['B', 'KB', 'MB', 'GB'];
    let value = Number(bytes);
    let i = 0;
    while (value >= 1024 && i < units.length - 1) { value /= 1024; i++; }
    return `${value.toFixed(value >= 10 || i === 0 ? 0 : 1)} ${units[i]}`;
  }

  function toast(message, kind = '') {
    const node = document.createElement('div');
    node.className = `toast ${kind}`;
    node.textContent = message;
    $('#toasts').appendChild(node);
    setTimeout(() => {
      node.style.opacity = '0';
      node.style.transition = 'opacity .2s';
      setTimeout(() => node.remove(), 220);
    }, 3200);
  }

  function nearBottom(el) {
    return el.scrollHeight - el.scrollTop - el.clientHeight < 120;
  }

  // ── 会话模型 ────────────────────────────────────────────

  function convoKeyOf(message) {
    if (message.kind === 'group') return `group:${message.groupId}`;
    if (message.kind === 'dm') {
      const other = message.userId === state.me?.userId ? message.receiverId : message.userId;
      return `dm:${other}`;
    }
    return 'public';
  }

  function ensureConvo(key, patch = {}) {
    if (!state.convos.has(key)) {
      const kind = key.split(':')[0];
      state.convos.set(key, {
        key,
        kind,
        id: key.includes(':') ? key.slice(key.indexOf(':') + 1) : '',
        title: kind === 'public' ? '公共大厅' : (patch.title || key),
        messages: [],
        byId: new Map(),      // 消息按 id 索引，避免每次操作都线性扫描
        unread: 0,
        loaded: false,
      });
    }
    const convo = state.convos.get(key);
    Object.assign(convo, patch);
    return convo;
  }

  function titleOf(key) {
    if (key === 'public') return '公共大厅';
    if (key.startsWith('dm:')) {
      const id = key.slice(3);
      return state.people.get(id)?.nickname || state.convos.get(key)?.title || '私聊';
    }
    if (key.startsWith('group:')) {
      const id = key.slice(6);
      return state.groups.find((g) => g.groupId === id)?.name || state.convos.get(key)?.title || '群组';
    }
    return key;
  }

  function pushMessage(key, message) {
    const convo = ensureConvo(key, { title: titleOf(key) });

    if (message.id) {
      const existing = convo.byId.get(message.id);
      if (existing) {
        Object.assign(existing, message);
        if (key === state.active) scheduleRender();
        return;
      }
    }

    convo.messages.push(message);
    if (message.id) convo.byId.set(message.id, message);

    // 绝大多数情况是新消息追加在末尾：直接插 DOM，不重建整张列表
    if (key === state.active && canAppend(message)) {
      appendMessageNode(convo, message);
      markReadSoon();
      return;
    }

    convo.messages.sort((a, b) => (a.ts - b.ts) || (a.id - b.id));
    if (convo.messages.length > 400) {
      const dropped = convo.messages.splice(0, convo.messages.length - 400);
      for (const item of dropped) convo.byId.delete(item.id);
    }

    if (key !== state.active) {
      convo.unread += 1;
      scheduleSidebar();
    } else {
      scheduleRender();
      markReadSoon();
    }
  }

  function normalize(raw, kind, extra = {}) {
    return {
      id: Number(raw.messageId ?? raw.id ?? 0),
      userId: raw.userId || raw.senderId || '',
      nickname: raw.nickname || raw.senderNickname || '匿名',
      receiverId: raw.receiverId || '',
      groupId: raw.groupId || extra.groupId || '',
      text: raw.text || '',
      ts: Number(raw.ts ?? raw.timestamp ?? Math.floor(Date.now() / 1000)),
      type: raw.messageType || (raw.fileUrl ? 'file' : 'text'),
      fileUrl: raw.fileUrl || '',
      fileName: raw.fileName || '',
      fileSize: Number(raw.fileSize || 0),
      isRecalled: !!raw.isRecalled,
      edited: !!raw.edited,
      kind,
      read: false,
    };
  }

  // ── WebSocket ───────────────────────────────────────────

  function wsUrl() {
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    return `${scheme}://${location.hostname}:${WS_PORT}/`;
  }

  function connect() {
    clearTimeout(state.retryTimer);
    setConnection('connecting');

    let ws;
    try {
      ws = new WebSocket(wsUrl());
    } catch {
      scheduleReconnect();
      return;
    }
    state.ws = ws;

    ws.onopen = () => {
      state.connected = true;
      state.retry = 0;
      setConnection('online');
      if (state.token) {
        send('resume_session', { sessionToken: state.token });
      } else if (state.me?.guest) {
        send('join', { nickname: state.me.nickname });
      }
    };

    ws.onmessage = (event) => {
      let payload;
      try { payload = JSON.parse(event.data); } catch { return; }
      handle(payload);
    };

    ws.onclose = () => {
      state.connected = false;
      setConnection('offline');
      scheduleReconnect();
    };

    ws.onerror = () => { /* onclose 会跟进处理 */ };
  }

  function scheduleReconnect() {
    const delay = Math.min(15000, 600 * Math.pow(1.7, state.retry++));
    clearTimeout(state.retryTimer);
    state.retryTimer = setTimeout(connect, delay);
  }

  function send(type, payload = {}) {
    if (!state.ws || state.ws.readyState !== WebSocket.OPEN) {
      toast('连接已断开，正在重连…', 'error');
      return false;
    }
    state.ws.send(JSON.stringify({ type, ...payload }));
    return true;
  }

  function setConnection(mode) {
    const dot = $('#conn-dot');
    const text = $('#conn-text');
    if (!dot) return;
    dot.classList.toggle('online', mode === 'online');
    text.textContent = mode === 'online' ? '已连接' : (mode === 'connecting' ? '连接中…' : '已断开，重连中…');
    $('#me-status').textContent = mode === 'online'
      ? (state.me?.guest ? '游客会话' : '在线')
      : '离线';
    $('#send-btn').disabled = mode !== 'online';
  }

  // ── 事件处理 ────────────────────────────────────────────

  function handle(event) {
    switch (event.type) {
      case 'logged_in':
        onLoggedIn(event);
        break;

      case 'joined':
        if (!state.me) {
          state.me = { userId: event.userId, nickname: event.nickname, guest: true, level: event.level || 0 };
        } else {
          state.me.userId = event.userId;
          state.me.nickname = event.nickname;
        }
        renderMe();
        // 游客没有登录返回，靠 joined 才能进入主界面
        if ($('#app').hidden) enterApp();
        break;

      case 'message':
        pushMessage('public', normalize(event, 'public'));
        break;

      case 'private_message': {
        const message = normalize(event, 'dm');
        const other = message.userId === state.me?.userId ? message.receiverId : message.userId;
        rememberPerson(other, message.nickname);
        rememberDm(other);
        pushMessage(`dm:${other}`, message);
        break;
      }

      case 'group_message': {
        const message = normalize(event, 'group');
        pushMessage(`group:${event.groupId}`, message);
        break;
      }

      case 'message_history': {
        const convo = ensureConvo('public', { title: '公共大厅' });
        mergeHistory(convo, (event.messages || []).map((m) => normalize(m, 'public')));
        break;
      }

      case 'private_message_history': {
        const other = event.otherUserId;
        rememberDm(other);
        const convo = ensureConvo(`dm:${other}`, { title: titleOf(`dm:${other}`) });
        mergeHistory(convo, (event.messages || []).map((m) => normalize(m, 'dm')));
        break;
      }

      case 'group_message_history': {
        const key = `group:${event.groupId}`;
        const convo = ensureConvo(key, { title: titleOf(key) });
        mergeHistory(convo, (event.messages || []).map((m) => normalize(m, 'group')));
        break;
      }

      case 'online_users':
        onOnlineUsers(event.users || []);
        break;

      case 'group_list':
        state.groups = event.groups || [];
        renderSidebar();
        if (state.active.startsWith('group:')) renderChatHead();
        break;

      case 'group_created':
        toast(`群组「${event.name}」已创建`, 'ok');
        send('get_groups');
        openConvo(`group:${event.groupId}`);
        break;

      case 'group_members':
        if (state.active === `group:${event.groupId}`) {
          state.members = event.members || [];
          renderMembers();
        }
        break;

      case 'typing':
        if (event.userId !== state.me?.userId) {
          state.typing.set(state.active, { nickname: event.nickname, at: Date.now() });
        }
        break;

      case 'message_recalled': {
        const convo = state.convos.get(state.active);
        const target = convo?.byId.get(Number(event.messageId));
        if (target) {
          target.isRecalled = true;
          target.text = '';
          scheduleRender();
        }
        break;
      }

      case 'message_edited': {
        for (const convo of state.convos.values()) {
          const target = convo.byId.get(Number(event.messageId));
          if (target) {
            target.text = event.text;
            target.edited = true;
            if (convo.key === state.active) scheduleRender();
            break;
          }
        }
        break;
      }

      case 'message_deleted': {
        for (const convo of state.convos.values()) {
          const id = Number(event.messageId);
          if (!convo.byId.has(id)) continue;
          convo.byId.delete(id);
          const index = convo.messages.findIndex((m) => m.id === id);
          if (index >= 0) convo.messages.splice(index, 1);
          if (convo.key === state.active) renderMessages();
          break;
        }
        break;
      }

      case 'message_read': {
        if (event.userId === state.me?.userId) break;
        const convo = state.convos.get(state.active);
        const target = convo?.byId.get(Number(event.messageId));
        if (target && target.userId === state.me?.userId) {
          target.read = true;
          markMessageRead(target.id);
        }
        break;
      }

      case 'message_pinned':
      case 'message_unpinned':
        toast(event.type === 'message_pinned' ? '已置顶' : '已取消置顶', 'ok');
        loadPinned();
        break;

      case 'pinned_messages':
        state.pinned = event.messages || [];
        renderPinned();
        break;

      case 'message_saved':
        toast('已收藏', 'ok');
        break;

      case 'saved_messages':
        if (event.messages?.length) toast(`共 ${event.messages.length} 条收藏`, 'ok');
        else toast('还没有收藏消息');
        break;

      case 'search_results':
        renderSearch(event.results || []);
        break;

      case 'user_info':
        rememberPerson(event.userId, event.nickname);
        if (event.online) {
          const person = state.people.get(event.userId);
          if (person) person.online = true;
        }
        renderSidebar();
        if (state.active === `dm:${event.userId}`) renderChatHead();
        break;

      case 'user_kicked':
      case 'user_banned':
        if (event.userId === state.me?.userId) toast('你已被移出该群组', 'error');
        break;

      case 'logged_out':
        onLogout(true);
        break;

      case 'error':
        onServerError(event);
        break;

      case 'pong':
      default:
        break;
    }
  }

  function mergeHistory(convo, incoming) {
    for (const message of incoming) {
      const key = message.id || `${message.userId}:${message.ts}`;
      const existing = convo.byId.get(key);
      if (existing) {
        Object.assign(existing, message);
      } else {
        convo.byId.set(key, message);
      }
    }

    convo.messages = Array.from(convo.byId.values())
      .sort((a, b) => (a.ts - b.ts) || (a.id - b.id));
    convo.loaded = true;
    if (convo.key === state.active) scheduleRender();
  }

  function onOnlineUsers(users) {
    const seen = new Set();
    for (const user of users) {
      seen.add(user.userId);
      const existing = state.people.get(user.userId) || {};
      state.people.set(user.userId, {
        ...existing,
        nickname: user.nickname,
        level: user.level,
        guest: user.guest,
        online: true,
      });
    }
    for (const [id, person] of state.people) {
      if (!seen.has(id)) person.online = false;
    }
    $('#online-count').textContent = `${users.length} 在线`;
    renderMembers();
    renderSidebar();
  }

  function rememberPerson(id, nickname) {
    if (!id) return;
    const existing = state.people.get(id) || {};
    state.people.set(id, { ...existing, nickname: existing.nickname || nickname || id });
  }

  function rememberDm(id) {
    if (!id || state.dmIds.includes(id)) return;
    state.dmIds.push(id);
    localStorage.setItem('im.dms', JSON.stringify(state.dmIds));
    renderSidebar();
  }

  function onServerError(event) {
    const code = event.code || '';
    const text = event.message || '操作失败';

    if (code === 'INVALID_CREDENTIALS' && !state.me && state.token) {
      state.token = '';
      localStorage.removeItem('im.token');
      showAuth();
      $('#auth-error').textContent = '登录状态已过期，请重新登录';
      return;
    }

    if (code === 'NOT_JOINED') {
      if (state.me?.guest) send('join', { nickname: state.me.nickname });
      return;
    }

    toast(text, 'error');
  }

  // ── 认证 ────────────────────────────────────────────────

  function onLoggedIn(event) {
    state.token = event.sessionToken || state.token;
    if (state.token) localStorage.setItem('im.token', state.token);

    state.me = {
      userId: event.userId,
      username: event.username || '',
      nickname: event.nickname || '我',
      level: event.level || 0,
      guest: false,
    };

    enterApp();
    send('join', { nickname: state.me.nickname });
    toast(event.resumed ? '已恢复上次登录' : '登录成功', 'ok');
  }

  function enterApp() {
    $('#auth').hidden = true;
    $('#app').hidden = false;
    renderMe();
    renderSidebar();
    openConvo('public');
    send('get_groups');
    send('get_online_users');
  }

  function renderMe() {
    if (!state.me) return;
    const me = $('#me-avatar');
    const name = state.me.nickname || '我';
    const hue = hueFor(state.me.userId || name);
    me.style.background = `linear-gradient(135deg,hsl(${hue} 62% 52%),hsl(${(hue + 40) % 360} 66% 58%))`;
    me.textContent = String(name).trim().slice(0, 2).toUpperCase();
    $('#me-name').textContent = name;
    $('#me-status').textContent = state.me.guest ? '游客会话' : '在线';
  }

  function showAuth() {
    $('#auth').hidden = false;
    $('#app').hidden = true;
    $('#auth-username').focus();
  }

  function onLogout(silent = false) {
    if (!silent) send('logout');
    state.token = '';
    state.me = null;
    localStorage.removeItem('im.token');
    state.convos.clear();
    state.people.clear();
    state.dmIds = [];
    localStorage.removeItem('im.dms');
    $('#messages').innerHTML = '';
    showAuth();
    if (!silent) toast('已退出登录');
  }

  // ── 渲染：侧栏 ──────────────────────────────────────────

  function renderSidebar() {
    // 私聊
    const dmHost = $('#dm-list');
    dmHost.innerHTML = state.dmIds.map((id) => {
      const key = `dm:${id}`;
      const person = state.people.get(id);
      const convo = state.convos.get(key);
      const unread = convo?.unread || 0;
      const active = state.active === key ? ' active' : '';
      return `<button class="convo${active}" data-key="${esc(key)}" type="button">
        ${avatarHtml(person?.nickname || id, id, 'sm')}
        <span class="convo-title">${esc(person?.nickname || id)}</span>
        ${unread ? `<span class="badge">${unread > 99 ? '99+' : unread}</span>` : ''}
      </button>`;
    }).join('') || '<p class="muted" style="padding:4px 10px">点击右上角 ＋ 发起私聊</p>';

    // 群组
    const groupHost = $('#group-list');
    groupHost.innerHTML = state.groups.map((group) => {
      const key = `group:${group.groupId}`;
      const convo = state.convos.get(key);
      const unread = convo?.unread || 0;
      const active = state.active === key ? ' active' : '';
      return `<button class="convo${active}" data-key="${esc(key)}" type="button">
        <span class="convo-icon">◇</span>
        <span class="convo-title">${esc(group.name)}</span>
        ${unread ? `<span class="badge">${unread > 99 ? '99+' : unread}</span>` : ''}
      </button>`;
    }).join('') || '<p class="muted" style="padding:4px 10px">还没有群组</p>';

    $$('.convo').forEach((button) => {
      button.classList.toggle('active', button.dataset.key === state.active);
      button.onclick = () => {
        openConvo(button.dataset.key);
        $('#sidebar').classList.remove('open');
      };
    });

    const publicUnread = state.convos.get('public')?.unread || 0;
    const badge = $('[data-unread="public"]');
    if (badge) {
      badge.hidden = publicUnread === 0;
      badge.textContent = publicUnread > 99 ? '99+' : publicUnread;
    }
  }

  function openConvo(key) {
    state.active = key;
    const convo = ensureConvo(key, { title: titleOf(key) });
    convo.unread = 0;

    renderSidebar();
    renderChatHead();
    renderMessages();

    if (!convo.loaded) {
      if (key === 'public') loadPublicHistory();
      else if (key.startsWith('dm:')) {
        const other = key.slice(3);
        if (!state.people.get(other)) send('get_user_info', { targetUserId: other });
        send('get_private_message_history', { otherUserId: other, limit: 80 });
      } else if (key.startsWith('group:')) {
        send('get_group_message_history', { groupId: key.slice(6), limit: 80 });
        send('get_group_members', { groupId: key.slice(6) });
      }
    }

    loadPinned();
    renderMembers();
    $('#message-input').focus();
    markReadSoon(true);
  }

  function renderChatHead() {
    const key = state.active;
    $('#chat-title').textContent = titleOf(key);

    if (key === 'public') {
      $('#chat-sub').textContent = '所有在线用户可见';
    } else if (key.startsWith('dm:')) {
      const person = state.people.get(key.slice(3));
      $('#chat-sub').textContent = person?.online ? '在线' : '私聊';
    } else {
      const group = state.groups.find((g) => g.groupId === key.slice(6));
      $('#chat-sub').textContent = group ? `${group.memberCount} 位成员` : '群组';
    }
  }

  // ── 渲染：消息 ──────────────────────────────────────────

  // 增量渲染状态：DOM 里当前是哪个会话、最后一条消息是谁
  const view = { key: null, lastId: 0, lastTs: 0, lastUserId: '', lastDay: '' };
  let renderScheduled = false;
  let sidebarScheduled = false;

  // 同一帧内的多条事件合并成一次渲染
  function scheduleRender() {
    if (renderScheduled) return;
    renderScheduled = true;
    requestAnimationFrame(() => {
      renderScheduled = false;
      renderMessages();
    });
  }

  function scheduleSidebar() {
    if (sidebarScheduled) return;
    sidebarScheduled = true;
    requestAnimationFrame(() => {
      sidebarScheduled = false;
      renderSidebar();
    });
  }

  function dayHtml(ts) {
    return `<div class="day">${fmtDay(ts)}</div>`;
  }

  // 只有「追加在末尾」才走增量路径，其它情况一律全量重建
  function canAppend(message) {
    const convo = state.convos.get(state.active);
    if (!convo || view.key !== convo.key) return false;
    if (!view.lastId) return false;                 // 列表还是空的
    if (message.id && message.id <= view.lastId) return false;
    if (message.ts < view.lastTs) return false;
    return true;
  }

  function appendMessageNode(convo, message) {
    const host = $('#messages');
    const stick = nearBottom(host);
    const day = dayKey(message.ts);
    const sameDay = day === view.lastDay;

    let html = sameDay ? '' : dayHtml(message.ts);
    const mine = message.userId === state.me?.userId;
    const continued = sameDay && view.lastUserId === message.userId &&
                      (message.ts - view.lastTs) < 300;
    html += msgHtml(message, mine, continued);

    host.insertAdjacentHTML('beforeend', html);

    view.lastId = message.id || view.lastId;
    view.lastTs = message.ts;
    view.lastUserId = message.userId;
    view.lastDay = day;

    if (stick) host.scrollTop = host.scrollHeight;
  }

  function renderMessages() {
    const host = $('#messages');
    const convo = state.convos.get(state.active);
    const stick = nearBottom(host);

    view.key = state.active;
    view.lastId = 0;
    view.lastTs = 0;
    view.lastUserId = '';
    view.lastDay = '';

    if (!convo || convo.messages.length === 0) {
      host.innerHTML = `<div class="muted" style="margin:auto;text-align:center;line-height:2">
        ${state.active === 'public' ? '还没有消息，来说第一句吧 👋' : '还没有聊天记录'}
      </div>`;
      return;
    }

    const myId = state.me?.userId;
    const parts = [];
    let lastDay = '';
    let prev = null;

    for (const message of convo.messages) {
      const day = dayKey(message.ts);
      if (day !== lastDay) {
        parts.push(dayHtml(message.ts));
        lastDay = day;
        prev = null;
      }

      const mine = message.userId === myId;
      const continued = prev && prev.userId === message.userId && (message.ts - prev.ts) < 300;

      parts.push(msgHtml(message, mine, continued));
      prev = message;
    }

    host.innerHTML = parts.join('');
    if (stick) host.scrollTop = host.scrollHeight;

    if (prev) {
      view.lastId = prev.id || 0;
      view.lastTs = prev.ts;
      view.lastUserId = prev.userId;
      view.lastDay = lastDay;
    }
  }

  // 已读回执只改一个 class，不重建列表
  function markMessageRead(id) {
    const node = document.querySelector(`#messages .msg[data-id="${id}"]`);
    if (node) node.classList.add('read');
  }

  function msgHtml(message, mine, continued) {
    const who = message.nickname || state.people.get(message.userId)?.nickname || '匿名';

    let content;
    if (message.isRecalled) {
      content = `<div class="bubble recalled">${mine ? '你撤回了一条消息' : `${esc(who)} 撤回了一条消息`}</div>`;
    } else if (message.type === 'file' && message.fileUrl) {
      const isImage = /\.(png|jpe?g|gif|webp)$/i.test(message.fileUrl);
      const thumb = isImage
        ? `<img class="thumb" src="${esc(message.fileUrl)}" alt="${esc(message.fileName)}" loading="lazy" onerror="this.remove()">`
        : '';
      content = `<div class="bubble">
        <a class="file-card" href="${esc(message.fileUrl)}" target="_blank" rel="noopener">
          ${thumb}
          <span class="file-icon">${isImage ? '🖼' : '📄'}</span>
          <span>
            <span class="file-name">${esc(message.fileName || '文件')}</span><br>
            <span class="file-size">${fmtBytes(message.fileSize)}</span>
          </span>
        </a>
        <span class="meta">${fmtTime(message.ts)}${message.edited ? ' · 已编辑' : ''}</span>
      </div>`;
    } else {
      // 含换行、代码块、列表等块级语法时走块级渲染，否则保持单行紧凑排版
      const blocky = /\n/.test(message.text) ||
        /(^|\n)\s*(?:```|#{1,6}\s|>\s|[-*+]\s|\d+[.)]\s)/.test(message.text);
      const body = blocky ? IMD.render(message.text) : IMD.renderInline(message.text);
      const meta = `<span class="meta">${fmtTime(message.ts)}${message.edited ? ' · 已编辑' : ''}</span>`;
      content = `<div class="bubble md${blocky ? ' md-block' : ''}">${body}${meta}</div>`;
    }

    const actions = message.isRecalled ? '' : `<div class="msg-actions">
      <button data-act="more" data-id="${message.id}" title="更多">⋯</button>
    </div>`;

    const head = continued ? '' : `<div class="msg-head">${mine ? '' : `<span class="who">${esc(who)}</span>`}</div>`;
    const readClass = mine && message.read ? ' read' : '';

    return `<div class="msg ${mine ? 'mine' : ''} ${continued ? 'cont' : ''}${readClass}" data-id="${message.id}" data-user="${esc(message.userId)}">
      ${mine ? '' : avatarHtml(who, message.userId, 'sm')}
      <div class="msg-body">${head}${content}</div>
      ${actions}
    </div>`;
  }

  // 事件全部委托到 #messages，避免每次渲染为几百个节点重新绑定
  function setupMessageDelegation() {
    $('#messages').addEventListener('click', async (event) => {
      const copy = event.target.closest('.md-copy');
      if (copy) {
        event.stopPropagation();
        const code = copy.closest('.md-pre')?.querySelector('code')?.textContent || '';
        try {
          await navigator.clipboard.writeText(code);
          copy.textContent = '已复制';
          setTimeout(() => { copy.textContent = '复制'; }, 1200);
        } catch {
          toast('复制失败，请手动选择', 'error');
        }
        return;
      }

      const more = event.target.closest('.msg-actions button');
      if (more) {
        event.stopPropagation();
        openContextMenu(more, Number(more.dataset.id));
      }
    });
  }

  // ── 消息操作 ────────────────────────────────────────────

  function findMessage(id) {
    return state.convos.get(state.active)?.byId.get(id) || null;
  }

  function openContextMenu(anchor, id) {
    const message = findMessage(id);
    if (!message) return;

    const mine = message.userId === state.me?.userId;
    const menu = $('#ctx-menu');
    const chatId = state.active === 'public' ? 'public' : (state.active.split(':')[1] || 'public');

    const items = [
      ['复制', () => navigator.clipboard.writeText(message.text || message.fileUrl || '').then(() => toast('已复制'))],
      ['置顶', () => send('pin_message', { messageId: id, chatId })],
      ['收藏', () => send('save_message', { messageId: id, chatId })],
    ];
    if (mine && !message.isRecalled && message.type !== 'file') {
      items.push(['编辑', () => editMessage(message)]);
    }
    if (mine && !message.isRecalled) {
      items.push(['撤回', () => send('recall_message', { messageId: id })]);
      items.push(['删除', () => send('delete_message', { messageId: id })]);
    }

    menu.innerHTML = items.map(([label], index) =>
      `<button data-index="${index}" class="${label === '删除' ? 'danger' : ''}">${label}</button>`).join('');
    menu.hidden = false;

    const rect = anchor.getBoundingClientRect();
    const width = menu.offsetWidth;
    const height = menu.offsetHeight;
    let left = Math.min(rect.left, window.innerWidth - width - 8);
    let top = rect.bottom + 6;
    if (top + height > window.innerHeight - 8) top = rect.top - height - 6;
    menu.style.left = `${Math.max(8, left)}px`;
    menu.style.top = `${Math.max(8, top)}px`;

    $$('#ctx-menu button').forEach((button) => {
      button.onclick = () => {
        menu.hidden = true;
        items[Number(button.dataset.index)][1]();
      };
    });
  }

  function editMessage(message) {
    openModal({
      title: '编辑消息',
      body: `<textarea id="edit-text" maxlength="${MAX_LEN}">${esc(message.text)}</textarea>`,
      onOk: () => {
        const text = $('#edit-text').value.trim();
        if (!text) return false;
        send('edit_message', { messageId: message.id, text });
        return true;
      },
    });
  }

  // ── 渲染：右栏 ──────────────────────────────────────────

  function renderMembers() {
    const host = $('#member-list');
    if (!host) return;

    const isGroup = state.active.startsWith('group:');
    const list = isGroup
      ? state.members.map((m) => ({
        id: m.userId,
        name: m.nickname,
        tag: m.isOwner ? '群主' : (m.isAdmin ? '管理员' : ''),
      }))
      : Array.from(state.people.entries())
        .filter(([, person]) => person.online)
        .map(([id, person]) => ({ id, name: person.nickname, tag: person.guest ? '游客' : '' }));

    host.innerHTML = list.map((item) => `
      <div class="member" data-user="${esc(item.id)}">
        ${avatarHtml(item.name, item.id, 'sm')}
        <span class="name">${esc(item.name)}</span>
        ${item.tag ? `<span class="tag">${esc(item.tag)}</span>` : ''}
      </div>`).join('') || '<p class="muted">暂无成员</p>';

    $$('#member-list .member').forEach((node) => {
      node.onclick = () => {
        const id = node.dataset.user;
        if (id && id !== state.me?.userId) openConvo(`dm:${id}`);
      };
    });
  }

  function loadPinned() {
    const chatId = state.active === 'public' ? 'public' : (state.active.split(':')[1] || 'public');
    send('get_pinned_messages', { chatId });
  }

  function renderPinned() {
    const host = $('#pinned-list');
    if (!state.pinned.length) {
      host.innerHTML = '<p class="muted">暂无置顶</p>';
      return;
    }
    host.innerHTML = state.pinned.map((message) => `
      <div class="pinned-item" data-id="${message.messageId}">
        <div class="who">${esc(message.nickname || '匿名')} · ${fmtTime(Number(message.ts || 0))}</div>
        <div>${esc(message.text || message.fileName || '')}</div>
      </div>`).join('');
  }

  // ── 搜索 ────────────────────────────────────────────────

  let searchTimer = null;

  function setupSearch() {
    $('#search-input').addEventListener('input', (event) => {
      const query = event.target.value.trim();
      clearTimeout(searchTimer);

      if (query.length < 2) {
        $('#search-results').hidden = true;
        return;
      }

      searchTimer = setTimeout(() => send('search_messages', { query, limit: 20 }), 320);
    });

    document.addEventListener('click', (event) => {
      if (!event.target.closest('.search')) $('#search-results').hidden = true;
    });
  }

  function renderSearch(results) {
    const host = $('#search-results');
    if (!results.length) {
      host.innerHTML = '<p class="muted" style="padding:8px">没有匹配的消息</p>';
      host.hidden = false;
      return;
    }

    host.innerHTML = results.map((hit, index) => `
      <button class="search-hit" data-index="${index}">
        <b>${esc(hit.senderNickname || '匿名')}</b> · ${fmtDay(Number(hit.timestamp))}
        <small>${esc((hit.text || '').slice(0, 80))}</small>
      </button>`).join('');
    host.hidden = false;

    $$('.search-hit', host).forEach((button) => {
      button.onclick = () => {
        const hit = results[Number(button.dataset.index)];
        let key = 'public';
        if (hit.messageType === 'group') {
          key = `group:${hit.chatId}`;
        } else if (hit.messageType === 'private') {
          const other = hit.senderId === state.me?.userId ? hit.chatId : hit.senderId;
          key = `dm:${other}`;
        }

        host.hidden = true;
        openConvo(key);
        setTimeout(() => {
          const node = $(`#messages .msg[data-id="${hit.messageId}"]`);
          if (node) {
            node.scrollIntoView({ block: 'center', behavior: 'smooth' });
            node.style.transition = 'background .4s';
            node.style.background = 'var(--accent-soft)';
            setTimeout(() => { node.style.background = ''; }, 1200);
          }
        }, 380);
      };
    });
  }

  // ── 弹窗 ────────────────────────────────────────────────

  let modalOk = null;

  function openModal({ title, body, okText = '确定', onOk }) {
    $('#modal-title').textContent = title;
    $('#modal-body').innerHTML = body;
    $('#modal-ok').textContent = okText;
    $('#modal').hidden = false;
    modalOk = onOk;

    const focusable = $('#modal-body input, #modal-body textarea');
    if (focusable) setTimeout(() => focusable.focus(), 40);
  }

  function closeModal() {
    $('#modal').hidden = true;
    modalOk = null;
  }

  // ── 输入区 ──────────────────────────────────────────────

  function setupComposer() {
    const input = $('#message-input');
    input.maxLength = MAX_LEN;

    const autosize = () => {
      input.style.height = 'auto';
      input.style.height = `${Math.min(140, input.scrollHeight)}px`;
    };

    input.addEventListener('input', () => {
      autosize();
      const length = Array.from(input.value).length;
      const counter = $('#counter');
      counter.textContent = length > 0 ? `${length}/${MAX_LEN}` : '';
      counter.classList.toggle('over', length > MAX_LEN);

      updatePreview();

      const now = Date.now();
      if (length > 0 && now - state.lastTypingSent > 2200) {
        state.lastTypingSent = now;
        send('typing');
      }
    });

    input.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
        event.preventDefault();
        $('#composer').requestSubmit();
      }
    });

    $('#composer').addEventListener('submit', (event) => {
      event.preventDefault();
      const text = input.value.replace(/\s+$/, '');
      if (!text.trim()) return;

      if (Array.from(text).length > MAX_LEN) {
        toast(`消息不能超过 ${MAX_LEN} 个字符`, 'error');
        return;
      }

      const ok = state.active === 'public'
        ? send('send', { text })
        : state.active.startsWith('dm:')
          ? send('private_message', { receiverId: state.active.slice(3), text })
          : send('send_group_message', { groupId: state.active.slice(6), text });

      if (!ok) return;

      input.value = '';
      autosize();
      $('#counter').textContent = '';
    });

    $('#attach-btn').onclick = () => $('#file-input').click();

    $('#preview-btn').onclick = () => {
      const panel = $('#preview');
      panel.hidden = !panel.hidden;
      $('#preview-btn').classList.toggle('active', !panel.hidden);
      updatePreview(true);
    };

    let previewTimer = null;

    // 预览是逐字触发的，必须节流，否则每敲一个键就要重排一次 Markdown
    function updatePreview(immediate = false) {
      const panel = $('#preview');
      if (panel.hidden) return;

      clearTimeout(previewTimer);
      const paint = () => {
        if (panel.hidden) return;
        const draft = input.value.trim();
        panel.innerHTML = draft
          ? IMD.render(draft)
          : '<p class="md-p" style="color:var(--muted)">输入内容后在这里预览 Markdown</p>';
      };

      if (immediate) paint();
      else previewTimer = setTimeout(paint, 120);
    }
    $('#file-input').addEventListener('change', async (event) => {
      const file = event.target.files?.[0];
      event.target.value = '';
      if (!file) return;

      if (file.size > Number(cfg.maxUploadBytes || 8 * 1024 * 1024)) {
        toast('文件超过大小限制', 'error');
        return;
      }

      toast(`正在上传 ${file.name}…`);
      try {
        const response = await fetch(`/api/upload?name=${encodeURIComponent(file.name)}`, {
          method: 'POST',
          body: file,
        });
        if (!response.ok) throw new Error('upload failed');
        const data = await response.json();

        // 文件消息只在公共大厅/群里支持（私聊文件走同样的 send_file + 无 receiver 字段）
        if (state.active !== 'public') {
          toast('当前仅公共大厅支持发送文件', 'error');
          return;
        }
        send('send_file', {
          text: '',
          fileUrl: data.url,
          fileName: data.name || file.name,
          fileSize: data.size || file.size,
        });
      } catch {
        toast('上传失败，请重试', 'error');
      }
    });
  }

  // ── 已读 ────────────────────────────────────────────────

  function markReadSoon(force = false) {
    if (state.active !== 'public') return;
    const now = Date.now();
    if (!force && now - state.lastReadSent < 2500) return;

    const convo = state.convos.get('public');
    const last = convo?.messages[convo.messages.length - 1];
    if (!last?.id) return;

    state.lastReadSent = now;
    send('mark_read', { messageId: last.id });
  }

  // ── 初始化 ──────────────────────────────────────────────

  function setupAuth() {
    let mode = 'login';

    $$('.tab').forEach((tab) => {
      tab.onclick = () => {
        mode = tab.dataset.tab;
        $$('.tab').forEach((t) => t.classList.toggle('active', t === tab));
        $('#auth-form').classList.toggle('register-mode', mode === 'register');
        $('#auth-submit').textContent = mode === 'register' ? '注册并登录' : '登录';
        $('#auth-error').textContent = '';
      };
    });

    $('#auth-form').addEventListener('submit', (event) => {
      event.preventDefault();
      const username = $('#auth-username').value.trim();
      const password = $('#auth-password').value;
      const nickname = $('#auth-nickname').value.trim() || username;

      if (!username || !password) {
        $('#auth-error').textContent = '请输入用户名和密码';
        return;
      }
      if (mode === 'register' && !nickname) {
        $('#auth-error').textContent = '请输入昵称';
        return;
      }

      $('#auth-submit').disabled = true;
      setTimeout(() => { $('#auth-submit').disabled = false; }, 1200);

      if (mode === 'register') send('register', { username, password, nickname, deviceName: 'web' });
      else send('login', { username, password, deviceName: 'web' });
    });

    $('#guest-btn').onclick = () => {
      openModal({
        title: '以游客身份进入',
        body: '<input id="guest-name" maxlength="32" placeholder="给自己起个昵称">',
        okText: '进入',
        onOk: () => {
          const name = $('#guest-name').value.trim();
          if (!name) return false;
          state.me = { userId: '', nickname: name, guest: true, level: 0 };
          send('join', { nickname: name });
          return true;
        },
      });
    };

    $('#logout-btn').onclick = () => onLogout();
  }

  function setupUI() {
    const savedTheme = localStorage.getItem('im.theme') || 'light';
    document.documentElement.dataset.theme = savedTheme;

    $('#theme-toggle').onclick = () => {
      const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
      document.documentElement.dataset.theme = next;
      localStorage.setItem('im.theme', next);
    };

    $('#menu-btn').onclick = () => $('#sidebar').classList.toggle('open');

    $('#members-btn').onclick = () => {
      if (state.active.startsWith('group:')) {
        send('get_group_members', { groupId: state.active.slice(6) });
        toast('成员列表已刷新');
      } else {
        renderMembers();
        toast('右侧栏显示在线成员');
      }
    };

    $('#pinned-btn').onclick = () => {
      loadPinned();
      toast('已刷新置顶消息');
    };

    $('#modal-cancel').onclick = closeModal;
    $('#modal-ok').onclick = () => {
      if (!modalOk || modalOk() !== false) closeModal();
    };
    $('#modal').addEventListener('click', (event) => {
      if (event.target.id === 'modal') closeModal();
    });

    document.addEventListener('click', (event) => {
      if (!event.target.closest('#ctx-menu')) $('#ctx-menu').hidden = true;
    });

    window.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') {
        closeModal();
        $('#ctx-menu').hidden = true;
        $('#search-results').hidden = true;
      }
    });

    $('#new-dm').onclick = () => {
      const candidates = Array.from(state.people.entries())
        .filter(([id, person]) => id !== state.me?.userId && person.online);

      if (!candidates.length) {
        toast('当前没有其他在线用户', 'error');
        return;
      }

      openModal({
        title: '发起私聊',
        body: `<div class="picker">${candidates.map(([id, person]) =>
          `<button type="button" data-user="${esc(id)}">${avatarHtml(person.nickname, id, 'sm')}${esc(person.nickname)}</button>`
        ).join('')}</div>`,
        onOk: () => true,
      });

      $$('#modal-body .picker button').forEach((button) => {
        button.onclick = () => {
          const id = button.dataset.user;
          closeModal();
          rememberDm(id);
          openConvo(`dm:${id}`);
        };
      });
    };

    $('#new-group').onclick = () => {
      openModal({
        title: '创建群组',
        body: '<input id="group-name" maxlength="32" placeholder="群组名称">',
        okText: '创建',
        onOk: () => {
          const name = $('#group-name').value.trim();
          if (!name) return false;
          send('create_group', { name });
          return true;
        },
      });
    };
  }

  function loadPublicHistory() {
    send('get_message_history', { limit: 80 });
  }

  // 定时清理"正在输入"提示
  setInterval(() => {
    const entry = state.typing.get(state.active);
    const host = $('#typing');
    if (!host) return;
    if (entry && Date.now() - entry.at < 3000) {
      host.textContent = `${entry.nickname} 正在输入…`;
    } else {
      if (entry) state.typing.delete(state.active);
      host.textContent = '';
    }
  }, 600);

  // 启动
  setupUI();
  setupAuth();
  setupComposer();
  setupSearch();
  setupMessageDelegation();

  // 供自动化测试/排查使用（只读引用，不影响业务）
  window.__im = { state, view };

  connect();

  if (!state.token) showAuth();
})();
