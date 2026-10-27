/* ─────────────────────────────────────────────────────────
   轻量 Markdown 渲染器（聊天场景专用）
   - 先转义再解析，任何用户输入都不可能变成 HTML 标签
   - 支持：围栏代码块、行内代码、粗体/斜体/删除线、链接、图片、
           标题、有序/无序列表、任务列表、引用、分隔线
   - 不支持的语法（表格、脚注等）按纯文本显示，不会破坏排版

   性能：结果按原文缓存（LRU），同一段文本只解析一次；
   没有 Markdown 特征的文本直接跳过全部正则。
   ───────────────────────────────────────────────────────── */
window.IMD = (function () {
  'use strict';

  const CACHE_LIMIT = 600;          // 约 1~2 MB，够覆盖一个会话窗口

  const inlineCache = new Map();
  const blockCache = new Map();

  function memoGet(cache, key) {
    return cache.get(key);
  }

  // FIFO 淘汰：命中时不做 LRU 提升。
  // 实测"命中即 delete+set"的代价和解析一条短消息差不多，缓存等于白做。
  function memoPut(cache, key, value) {
    if (cache.size >= CACHE_LIMIT) {
      const oldest = cache.keys().next().value;
      if (oldest !== undefined) cache.delete(oldest);
    }
    cache.set(key, value);
  }

  // ── 转义 ──────────────────────────────────────────────

  const ESCAPE_TEST = /[&<>"']/;
  const ESCAPE_ALL = /[&<>"']/g;
  const ESCAPE_MAP = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
  const escapeChar = (c) => ESCAPE_MAP[c];

  function escapeHtml(value) {
    const text = value == null ? '' : String(value);
    return ESCAPE_TEST.test(text) ? text.replace(ESCAPE_ALL, escapeChar) : text;
  }

  function safeUrl(raw) {
    const url = String(raw || '').trim();
    if (!url) return '';
    if (/^https?:\/\//i.test(url)) return url;
    if (/^mailto:/i.test(url)) return url;
    if (url.startsWith('/uploads/')) return url;
    if (url.startsWith('#')) return url;
    return '';
  }

  // ── 行内 ──────────────────────────────────────────────

  // 没有这些字符就不必跑正则（大多数聊天消息走这条快路径）
  const INLINE_HINT = /[\n`*~[\]!]|https?:\/\//;

  function renderInlineUncached(raw) {
    const source = raw == null ? '' : String(raw);
    if (!INLINE_HINT.test(source)) return escapeHtml(source);

    let text = escapeHtml(source);
    const stash = [];

    const keep = (html) => {
      stash.push(html);
      return `\u0001${stash.length - 1}\u0001`;
    };

    // 行内代码优先占位，避免里面的 * _ ` 被当成格式
    text = text.replace(/`([^`\n]+)`/g, (_, code) =>
      keep(`<code class="md-inline-code">${code}</code>`));

    // 图片
    text = text.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, (whole, alt, url) => {
      const safe = safeUrl(url);
      if (!safe) return whole;
      return keep(`<img class="md-img" src="${safe}" alt="${alt}" loading="lazy" onerror="this.remove()">`);
    });

    // 链接
    text = text.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (whole, label, url) => {
      const safe = safeUrl(url);
      if (!safe) return `${label}（${url}）`;
      return keep(`<a class="md-link" href="${safe}" target="_blank" rel="noopener noreferrer">${label}</a>`);
    });

    // 强调（不处理下划线，避免把 foo_bar_baz 这类标识符拆开）
    text = text
      .replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>')
      .replace(/~~([^~\n]+)~~/g, '<del>$1</del>')
      .replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, '$1<em>$2</em>');

    // 裸链接（此时文本里除了占位符没有别的标签）
    text = text.replace(/https?:\/\/[^\s<>\u0001]+/g, (whole) => {
      const trailing = /[.,;:!?)\]}'"]+$/.exec(whole);
      const url = trailing ? whole.slice(0, -trailing[0].length) : whole;
      const tail = trailing ? trailing[0] : '';
      return `<a class="md-link" href="${url}" target="_blank" rel="noopener noreferrer">${url}</a>${tail}`;
    });

    // 换行
    text = text.replace(/\n/g, '<br>');

    return text.replace(/\u0001(\d+)\u0001/g, (_, index) => stash[Number(index)] ?? '');
  }

  function renderInline(raw) {
    const key = String(raw ?? '');
    const cached = memoGet(inlineCache, key);
    if (cached !== undefined) return cached;

    const html = renderInlineUncached(key);
    memoPut(inlineCache, key, html);
    return html;
  }

  // ── 块级 ──────────────────────────────────────────────

  const RE_FENCE = /^\s*```([A-Za-z0-9_+#.-]*)\s*$/;
  const RE_HEADING = /^(#{1,6})\s+(.+?)\s*#*\s*$/;
  const RE_HR = /^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/;
  const RE_QUOTE = /^\s*>\s?/;
  const RE_UL = /^\s*[-*+]\s+(.*)$/;
  const RE_OL = /^\s*\d+[.)]\s+(.*)$/;

  function startsBlock(line) {
    return RE_FENCE.test(line) || RE_HEADING.test(line) || RE_HR.test(line) ||
           RE_QUOTE.test(line) || RE_UL.test(line) || RE_OL.test(line);
  }

  function codeBlock(lang, body) {
    const label = escapeHtml(lang || 'code');
    return `<div class="md-pre">` +
      `<div class="md-pre-head"><span class="md-lang">${label}</span>` +
      `<button class="md-copy" type="button" title="复制代码">复制</button></div>` +
      `<pre><code>${escapeHtml(body)}</code></pre></div>`;
  }

  function listBlock(items, ordered) {
    const tag = ordered ? 'ol' : 'ul';
    let body = '';
    for (const item of items) {
      const task = /^\[([ xX])\]\s+([\s\S]*)$/.exec(item);
      if (task) {
        const checked = task[1].toLowerCase() === 'x';
        body += `<li class="md-li md-task"><span class="md-check${checked ? ' done' : ''}">${checked ? '☑' : '☐'}</span><span>${renderInline(task[2])}</span></li>`;
      } else {
        body += `<li class="md-li">${renderInline(item)}</li>`;
      }
    }
    return `<${tag} class="md-list">${body}</${tag}>`;
  }

  function renderBlockUncached(source) {
    const text = source.replace(/\r\n?/g, '\n').replace(/\t/g, '    ');
    const lines = text.split('\n');
    const out = [];
    let i = 0;

    while (i < lines.length) {
      const line = lines[i];

      // 围栏代码块
      const fence = RE_FENCE.exec(line);
      if (fence) {
        const body = [];
        i++;
        while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) {
          body.push(lines[i]);
          i++;
        }
        if (i < lines.length) i++;      // 跳过结束围栏
        out.push(codeBlock(fence[1], body.join('\n')));
        continue;
      }

      if (!line.trim()) { i++; continue; }

      // 分隔线
      if (RE_HR.test(line)) {
        out.push('<hr class="md-hr">');
        i++;
        continue;
      }

      // 标题
      const heading = RE_HEADING.exec(line);
      if (heading) {
        const level = Math.min(6, heading[1].length);
        out.push(`<h${level} class="md-h md-h${level}">${renderInline(heading[2])}</h${level}>`);
        i++;
        continue;
      }

      // 引用
      if (RE_QUOTE.test(line)) {
        const quoted = [];
        while (i < lines.length && RE_QUOTE.test(lines[i])) {
          quoted.push(lines[i].replace(RE_QUOTE, ''));
          i++;
        }
        out.push(`<blockquote class="md-quote">${renderInline(quoted.join('\n'))}</blockquote>`);
        continue;
      }

      // 列表
      const ul = RE_UL.exec(line);
      const ol = RE_OL.exec(line);
      if (ul || ol) {
        const ordered = Boolean(ol);
        const items = [];
        while (i < lines.length) {
          const m = ordered ? RE_OL.exec(lines[i]) : RE_UL.exec(lines[i]);
          if (m) {
            items.push(m[1]);
            i++;
            continue;
          }
          // 缩进续行并入上一项
          if (items.length && /^\s{2,}\S/.test(lines[i])) {
            items[items.length - 1] += '\n' + lines[i].trim();
            i++;
            continue;
          }
          break;
        }
        out.push(listBlock(items, ordered));
        continue;
      }

      // 段落：连续非空、非块起始的行
      const paragraph = [];
      while (i < lines.length && lines[i].trim() && !startsBlock(lines[i])) {
        paragraph.push(lines[i]);
        i++;
      }
      out.push(`<p class="md-p">${renderInline(paragraph.join('\n'))}</p>`);
    }

    return out.join('');
  }

  function render(source) {
    const key = String(source ?? '');
    const cached = memoGet(blockCache, key);
    if (cached !== undefined) return cached;

    const html = renderBlockUncached(key);
    memoPut(blockCache, key, html);
    return html;
  }

  function clearCache() {
    inlineCache.clear();
    blockCache.clear();
  }

  return { render, renderInline, escapeHtml, clearCache };
})();
