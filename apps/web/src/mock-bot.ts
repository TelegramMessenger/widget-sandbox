// The dev app's mock bot (docs/sdk-v1.md §2.13–§2.15): what a client and the server do around a widget's data and
// requests, in this trusted page. It keeps the message's `chatData` / `publicData` per file (as the server keeps
// them per message), answers the widget's requests with a built-in bot (the widget names it with
// `<meta name="tw-bot" content="poll">`, the panel can switch it), applies the bot's edits as the server's update
// (`setData` to every live host, before the answer), and sets the context a client passes: the server's flags for a
// kind of message (`WIDGET_MESSAGE_KINDS`), the chat type, the viewer. Dev tooling, not a product path.

import type { ChatType, WidgetContext, WidgetData, WidgetRequest, WidgetRequestAnswer } from '@telegram/widget-sandbox/host/widget-host';
import { CHAT_TYPES, MAX_WIDGET_DATA_BYTES, WIDGET_MESSAGE_KINDS, utf8Length, viewerFromList, type WidgetMessageKind } from '@telegram/widget-sandbox/shared/sdk-policy';

type Data = { chatData: string | null; publicData: string | null };
/** A bot's work on a request: `result` for the widget; a data field that is present is its edit (`editMessageWidgetData`, null clears it). */
interface Reply { result?: string | null; chatData?: string | null; publicData?: string | null }
interface Bot {
  title: string;
  /** The data the bot sent the widget with. */
  initial: Data;
  answer(data: string, ctx: { viewer: number; data: Data }): Reply;
}

/** The dev app's viewer besides the id (1001): first name, last name, username, premium (`viewerFromList`). */
export const DEFAULT_PROFILE = 'Ann,Lee,ann_lee';
const json = (s: string | null, fallback: any) => { try { return s === null ? fallback : JSON.parse(s); } catch { return fallback; } };

const LINES = [[0, 1, 2], [3, 4, 5], [6, 7, 8], [0, 3, 6], [1, 4, 7], [2, 5, 8], [0, 4, 8], [2, 4, 6]];
const winner = (b: string) => LINES.find(([a, c, d]) => b[a] !== '-' && b[a] === b[c] && b[a] === b[d])?.map((i) => b[i])[0] ?? (b.includes('-') ? null : 'draw');
/** The bot's move: win, block, centre, a corner, anything. */
function botMove(b: string): number {
  for (const mark of ['o', 'x']) {
    for (const line of LINES) {
      const marks = line.map((i) => b[i]);
      if (marks.filter((m) => m === mark).length === 2 && marks.includes('-')) return line[marks.indexOf('-')];
    }
  }
  return [4, 0, 2, 6, 8, 1, 3, 5, 7].find((i) => b[i] === '-') ?? -1;
}

export const BOTS: Record<string, Bot> = {
  echo: {
    title: 'echo: result = the request data',
    initial: { chatData: null, publicData: null },
    answer: (data) => ({ result: data }),
  },
  poll: {
    title: 'poll: vote:N toggles the viewer\'s vote; chatData {v: counts, u: {viewer: option}}, publicData {v}',
    initial: { chatData: '{"v":[3,5,1],"u":{"7":0,"8":1,"9":1}}', publicData: '{"v":[3,5,1]}' },
    answer(data, { viewer, data: d }) {
      const m = /^vote:([0-9])$/.exec(data);
      if (!m) return { result: 'unknown' };
      const state = json(d.chatData, { v: [0, 0, 0], u: {} });
      const option = Number(m[1]);
      if (!(option < state.v.length)) return { result: 'unknown' };
      const before = state.u[viewer];
      if (before !== undefined) state.v[before]--;
      if (before === option) delete state.u[viewer];
      else { state.u[viewer] = option; state.v[option]++; }
      return { result: JSON.stringify({ voted: state.u[viewer] ?? null }), chatData: JSON.stringify(state), publicData: JSON.stringify({ v: state.v }) };
    },
  },
  tictactoe: {
    title: 'tic-tac-toe: move:N plays X and the bot answers O; reset; chatData {b, s}, publicData = the last game',
    initial: { chatData: '{"b":"---------","s":"play"}', publicData: null },
    answer(data, { data: d }) {
      if (data === 'reset') return { result: 'new game', chatData: '{"b":"---------","s":"play"}' };
      const m = /^move:([0-8])$/.exec(data);
      const game = json(d.chatData, { b: '---------', s: 'play' });
      if (!m || game.s !== 'play' || game.b[Number(m[1])] !== '-') return { result: 'invalid move' };
      let b = game.b.slice(0, Number(m[1])) + 'x' + game.b.slice(Number(m[1]) + 1);
      let w = winner(b);
      if (!w) { const o = botMove(b); b = b.slice(0, o) + 'o' + b.slice(o + 1); w = winner(b); }
      const next = JSON.stringify({ b, s: w ?? 'play' });
      return { result: w ? (w === 'draw' ? 'draw' : w + ' wins') : 'your move', chatData: next, ...(w ? { publicData: next } : {}) };
    },
  },
  ticker: {
    title: 'ticker: tick (background) → {t, p}; like → chatData {likes}',
    initial: { chatData: '{"likes":0}', publicData: null },
    answer(data, { data: d }) {
      if (data === 'like') return { result: 'liked', chatData: JSON.stringify({ likes: (json(d.chatData, {}).likes ?? 0) + 1 }) };
      const p = 100 + 10 * Math.sin(Date.now() / 20000) + Math.random();
      return { result: JSON.stringify({ t: new Date().toISOString().slice(11, 19), p: Math.round(p * 100) / 100 }) };
    },
  },
  promo: {
    title: 'promo: redeem once; publicData {code, off}, chatData {redeemed}',
    initial: { chatData: null, publicData: '{"code":"TG-WIDGET-2026","off":20}' },
    answer(data, { data: d }) {
      if (data !== 'redeem') return { result: 'unknown' };
      if (json(d.chatData, {}).redeemed) return { result: 'already' };
      return { result: 'ok', chatData: JSON.stringify({ redeemed: new Date().toISOString().slice(0, 16) }) };
    },
  },
};

/** How the mock bot answers: with the selected bot, or as a client's failures; `manual` keeps the request until a button answers it. */
export const MODES = ['bot', 'manual', 'TIMEOUT', 'RATE_LIMITED', 'FAILED'] as const;
export type Mode = (typeof MODES)[number];

export interface RequestRecord {
  at: string;
  file: string | null;
  data: string;
  /** What went back to the host: the answer, or a failure code; undefined while pending. */
  answer?: WidgetRequestAnswer;
  error?: string;
}

/** The bot named by `<meta name="tw-bot" content="…">` in the widget, `echo` otherwise. */
export function botOf(html: string): string {
  const m = /<meta\s+name=["']tw-bot["']\s+content=["']([\w-]+)["']/i.exec(html);
  return m && BOTS[m[1]] ? m[1] : 'echo';
}

export interface MockBotHooks {
  /** The context changed: the widget is re-created (a host's context is fixed for its life). */
  restart(): void;
  /** The bot edited the data: every live host of the file gets it (the server's update, or the polled shared object). */
  setData(data: Data): void;
  /** Something the panel shows changed. */
  changed(): void;
}

export class MockBot {
  bot = 'echo';
  mode: Mode = 'bot';
  delayMs = 300;
  /**
   * The kind of message (the server's flags), its chat type (null: the kind's own), the viewer's id and the rest of
   * the viewer as `first,last,username,premium` (`viewerFromList`; the host checks it).
   */
  context: { message: WidgetMessageKind; chatType: ChatType | null; viewer: number | null; profile: string } =
    { message: 'private', chatType: null, viewer: 1001, profile: DEFAULT_PROFILE };
  /**
   * The widget is a shared object (its message has a `widget_id`): the client polls its `public_data` while it is on
   * screen and watches (`TelegramWidget.watchPublicData`), every `pollMs` (the server's period: 5–3600 s,
   * default 30 s, here shorter for the dev app). Without it nothing is polled.
   */
  shared = true;
  pollMs = 5000;
  /** The live widget watches its public_data (the host's `onWatchPublicData`); the panel shows it. */
  watching = false;
  /** Polls made, and the ones that brought a new value. */
  polls = { made: 0, changed: 0 };
  readonly requests: RequestRecord[] = [];
  private data = new Map<string, Data>();
  /** The data the live widget has: what it got with the message, a pushed edit or a poll. */
  private delivered = new Map<string, Data>();
  private pending = new Map<RequestRecord, (outcome: { answer?: WidgetRequestAnswer; error?: string }) => void>();

  constructor(readonly hooks: MockBotHooks) {}

  /** The message's data as the "server" has it now (the bot's initial data the first time). */
  dataFor(file: string | null): Data {
    const key = file ?? '';
    if (!this.data.has(key)) this.data.set(key, { ...BOTS[this.bot].initial });
    return this.data.get(key)!;
  }

  /** A fresh message: the bot's initial data again. */
  resetData(file: string | null) {
    this.data.delete(file ?? '');
  }

  /** The client created a host with the message's data (`dataFor`): that is what the widget has now. */
  loaded(file: string | null) {
    this.delivered.set(file ?? '', this.dataFor(file));
    this.watching = false;
  }

  /**
   * The bot's `editWidgetData` (a shared object by its id): the server's value changes, nothing is pushed; the widget
   * gets it at the client's next poll if it watches, or with the message the next time it is shown.
   */
  editShared(file: string | null, publicData: string | null) {
    this.data.set(file ?? '', { ...this.dataFor(file), publicData });
    this.hooks.changed();
  }

  /** The client's poll (`getWidgetPublicData`): the full state if the shared value changed since the widget got it, else null. */
  poll(file: string | null): Data | null {
    if (!this.shared) return null;
    this.polls.made++;
    const key = file ?? '';
    const now = this.dataFor(file), had = this.delivered.get(key);
    if (had && had.publicData === now.publicData) return null;
    this.polls.changed++;
    const next = { chatData: had?.chatData ?? now.chatData, publicData: now.publicData };
    this.delivered.set(key, next);
    this.hooks.changed();
    return next;
  }

  hostContext(): WidgetContext {
    const c = this.context;
    const kind = WIDGET_MESSAGE_KINDS[c.message];
    const viewer = c.viewer !== null ? viewerFromList(`${c.viewer},${c.profile}`) : null;
    return { ...kind, chatType: c.chatType ?? kind.chatType, ...(viewer ? { viewer } : {}) };
  }

  /** The bot's edit (`editMessageWidgetData`): the full state of both strings to every live host. */
  edit(file: string | null, data: Data) {
    this.data.set(file ?? '', data);
    this.delivered.set(file ?? '', data);
    this.hooks.setData(data);
    this.hooks.changed();
  }

  /** `WidgetHostOptions.onRequest`: what the client would send over MTProto and get back. */
  request(file: string | null, req: WidgetRequest): Promise<WidgetRequestAnswer> {
    const rec: RequestRecord = { at: new Date().toISOString().slice(11, 23), file, data: req.data };
    this.requests.push(rec);
    if (this.requests.length > 200) this.requests.shift();
    this.hooks.changed();
    return new Promise<WidgetRequestAnswer>((resolve, reject) => {
      const settle = (o: { answer?: WidgetRequestAnswer; error?: string }) => {
        this.pending.delete(rec);
        rec.answer = o.answer;
        rec.error = o.error;
        this.hooks.changed();
        if (o.error) reject(o.error);
        else resolve(o.answer!);
      };
      this.pending.set(rec, settle);
      if (this.mode === 'manual') return;
      const mode = this.mode;
      setTimeout(() => (mode === 'bot' ? this.answer(rec) : this.fail(rec, mode)), this.delayMs);
    });
  }

  /**
   * Answers a pending request with the selected bot, as a bot does: it edits its data with a separate call (which
   * reaches every live host as the server's update, here before the answer), then answers with `result` only.
   */
  answer(rec: RequestRecord) {
    const settle = this.pending.get(rec);
    if (!settle) return;
    const data = this.dataFor(rec.file);
    const reply = BOTS[this.bot].answer(rec.data, { viewer: this.context.viewer ?? 0, data });
    if ('chatData' in reply || 'publicData' in reply) {
      this.edit(rec.file, { chatData: 'chatData' in reply ? reply.chatData || null : data.chatData, publicData: 'publicData' in reply ? reply.publicData || null : data.publicData });
    }
    settle({ answer: { result: reply.result ?? null } });
  }

  fail(rec: RequestRecord, code: string) {
    this.pending.get(rec)?.({ error: code });
  }

  get pendingRequests(): RequestRecord[] {
    return [...this.pending.keys()];
  }
}

// ───────────── the panel ─────────────

function make<T extends HTMLElement = HTMLElement>(tag: string, props: Record<string, unknown> = {}, ...children: (Node | string)[]): T {
  const e = Object.assign(document.createElement(tag), props) as T;
  e.append(...children);
  return e;
}
const bytes = (s: string) => `${utf8Length(s)} / ${MAX_WIDGET_DATA_BYTES} B`;

/** The right column's "Bot (mock)" section: context, the data and its edit, the answer mode, the requests. */
export function mountPanel(root: HTMLElement, bot: MockBot, file: () => string | null) {
  const message = make<HTMLSelectElement>('select', { title: 'the kind of message, and so the server\'s flags: private = the original in the bot\'s private chat (taps and background requests); group, via-bot = taps only; no-queries = an original sent without allow_queries (no requests, no network marker); live-copy = a forward of a widget sent with fwd_allow_queries (taps only); copy = a forward without it (no requests); preview = an inline result not sent yet. Re-creates the widget' },
    ...Object.entries(WIDGET_MESSAGE_KINDS).map(([k, f]) => make('option', { value: k, textContent: k, title: `isCopy ${f.isCopy}, canQuery ${f.canQuery}, backgroundQuery ${f.backgroundQuery}` })));
  const chatType = make<HTMLSelectElement>('select', { title: 'the chat kind the client reports; default: the message kind\'s (re-creates the widget)' }, ...['', ...CHAT_TYPES].map((t) => make('option', { value: t, textContent: t || '(the kind\'s)' })));
  const viewer = make<HTMLInputElement>('input', { type: 'number', min: '1', title: 'the viewer\'s user id; empty: none (re-creates the widget)' });
  const profile = make<HTMLInputElement>('input', { type: 'text', size: 18, placeholder: 'first,last,username,premium', title: 'the rest of the viewer (WebAppUser): first name, last name, username, premium; empty parts are absent; the host leaves out a field that breaks its rule (data-rejected). Re-creates the widget' });
  const botSelect = make<HTMLSelectElement>('select', { title: 'which built-in bot answers; a widget names its own with <meta name="tw-bot">' }, ...Object.entries(BOTS).map(([k, b]) => make('option', { value: k, textContent: k, title: b.title })));
  const mode = make<HTMLSelectElement>('select', { title: 'bot: the bot answers after the delay; manual: answer each request below; the rest fail as a client reports it' }, ...MODES.map((m) => make('option', { value: m, textContent: m })));
  const delay = make<HTMLInputElement>('input', { type: 'number', min: '0', step: '100', title: 'how long the bot takes to answer (ms)' });
  const chatData = make<HTMLTextAreaElement>('textarea', { rows: 2, spellcheck: false, placeholder: 'chatData (null)' });
  const publicData = make<HTMLTextAreaElement>('textarea', { rows: 2, spellcheck: false, placeholder: 'publicData (null)' });
  const size = make('span', { className: 'muted' });
  const apply = make('button', { type: 'button', textContent: 'edit data', title: 'the bot\'s editMessageWidgetData: both strings to the widget (an empty field is none)' });
  const reset = make('button', { type: 'button', textContent: 'reset', title: 'the bot\'s initial data again (a fresh message; re-creates the widget)' });
  const byId = make('button', { type: 'button', textContent: 'edit by id', title: 'the bot\'s editWidgetData: only publicData, in the shared object; nothing is pushed: the widget gets it at the next poll if it watches (watchPublicData), else with the message next time' });
  const shared = make<HTMLInputElement>('input', { type: 'checkbox', title: 'the widget is a shared object (its message has a widget_id): the client polls its publicData while it watches' });
  const period = make<HTMLInputElement>('input', { type: 'number', min: '100', step: '500', title: 'the poll period in ms (the server\'s period: 5–3600 s, 30 s by default; shorter here)' });
  const polling = make('span', { className: 'muted' });
  const list = make('ul', { id: 'bot-requests' });
  const title = make('div', { className: 'muted bot-title' });
  root.replaceChildren(
    make('div', { className: 'bot-row' }, make('label', {}, 'message ', message), make('label', {}, 'chat ', chatType), make('label', {}, 'viewer ', viewer), make('label', {}, 'as ', profile)),
    make('div', { className: 'bot-row' }, make('label', {}, 'bot ', botSelect), make('label', {}, 'answers ', mode), make('label', {}, delay, ' ms')),
    title, chatData, publicData,
    make('div', { className: 'bot-row' }, apply, byId, reset, size),
    make('div', { className: 'bot-row' }, make('label', {}, shared, ' shared (widget_id)'), make('label', {}, 'poll ', period, ' ms'), polling),
    list,
  );

  const fill = () => {
    const c = bot.context;
    message.value = c.message;
    chatType.value = c.chatType ?? '';
    viewer.value = c.viewer === null ? '' : String(c.viewer);
    profile.value = c.profile;
    botSelect.value = bot.bot;
    mode.value = bot.mode;
    delay.value = String(bot.delayMs);
    title.textContent = BOTS[bot.bot].title;
    const d = bot.dataFor(file());
    if (document.activeElement !== chatData) chatData.value = d.chatData ?? '';
    if (document.activeElement !== publicData) publicData.value = d.publicData ?? '';
    size.textContent = `chatData ${bytes(chatData.value)}, publicData ${bytes(publicData.value)}`;
    shared.checked = bot.shared;
    if (document.activeElement !== period) period.value = String(bot.pollMs);
    polling.textContent = !bot.shared ? 'not shared: never polled' : bot.watching ? `watching: polled ${bot.polls.made}×, ${bot.polls.changed} new` : 'not watching: not polled';
  };
  const renderRequests = () => {
    list.replaceChildren(...bot.requests.slice(-12).reverse().map((r) => {
      const outcome = r.error ? `✖ ${r.error}` : r.answer ? `→ ${JSON.stringify(r.answer.result)}` : '… pending';
      const li = make('li', { title: r.answer ? JSON.stringify(r.answer) : '' }, `${r.at} ${JSON.stringify(r.data)} ${outcome}`);
      if (!r.answer && !r.error && bot.pendingRequests.includes(r)) {
        li.append(' ', make('button', { type: 'button', textContent: 'answer', onclick: () => bot.answer(r) }), ' ', make('button', { type: 'button', textContent: 'timeout', onclick: () => bot.fail(r, 'TIMEOUT') }));
      }
      return li;
    }));
  };
  const restartWith = (patch: Partial<MockBot['context']>) => { Object.assign(bot.context, patch); bot.hooks.restart(); bot.hooks.changed(); };
  message.addEventListener('change', () => restartWith({ message: message.value as WidgetMessageKind }));
  chatType.addEventListener('change', () => restartWith({ chatType: (chatType.value || null) as ChatType | null }));
  viewer.addEventListener('change', () => restartWith({ viewer: viewer.value ? Number(viewer.value) : null }));
  profile.addEventListener('change', () => restartWith({ profile: profile.value }));
  botSelect.addEventListener('change', () => { bot.bot = botSelect.value; fill(); });
  mode.addEventListener('change', () => { bot.mode = mode.value as Mode; bot.hooks.changed(); });
  delay.addEventListener('change', () => { bot.delayMs = Math.max(0, Number(delay.value) || 0); });
  for (const t of [chatData, publicData]) t.addEventListener('input', () => { size.textContent = `chatData ${bytes(chatData.value)}, publicData ${bytes(publicData.value)}`; });
  apply.addEventListener('click', () => bot.edit(file(), { chatData: chatData.value || null, publicData: publicData.value || null }));
  reset.addEventListener('click', () => { bot.resetData(file()); chatData.value = ''; publicData.value = ''; bot.hooks.restart(); });
  byId.addEventListener('click', () => bot.editShared(file(), publicData.value || null));
  shared.addEventListener('change', () => { bot.shared = shared.checked; bot.hooks.changed(); });
  period.addEventListener('change', () => { bot.pollMs = Math.max(100, Number(period.value) || 5000); bot.hooks.changed(); });
  return { render: () => { fill(); renderRequests(); } };
}

/** What a client shows after `copyText`: the copied text itself (anti-swap), over the bubble for a moment. */
export function showCopied(bubble: HTMLElement, text: string) {
  const tag = make('div', { className: 'bubble-gesture bubble-copied', textContent: `Copied: ${text}` });
  bubble.querySelector('.bubble-copied')?.remove();
  bubble.appendChild(tag);
  setTimeout(() => tag.remove(), 2000);
}

export type { Data as MockBotData, WidgetData };
