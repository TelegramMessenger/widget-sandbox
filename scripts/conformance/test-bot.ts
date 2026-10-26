// The reference test bot for the conformance widgets that talk to their bot (layer 233). It reads
// packages/conformance/channel.json: `bot` (what to do on a widget_query, by its exact data) and the posts'
// `message` (how a widget is sent). Bot API, long polling, no dependencies.
//
//   run (default): answers every widget_query by the `bot` table: edits the message's state
//     (editMessageWidgetData, by the message's coordinates or its inline_message_id) and the shared widget's
//     (editWidgetData by widget_id, with cache_time), repeats them when the action says so, then
//     answerWidgetQuery with the result. Templates: {n} counts the requests with that data per message (per
//     widget_id for a copy, which has no message), {from} is who pressed (first_name), {time} the bot's HH:MM:SS.
//     A query with data the table does not know gets an empty answer. It also answers the inline query
//     `conf-preview` with the widgets whose message spec says inline (the preview cases).
//   --post <widget id> [--variant <kind>]: sends that widget as its message specs say: sendWidget with the html,
//     the flags (allow_queries, fwd_allow_queries, shared_widget) and the state (chat_data, public_data), into
//     the tester's private chat or the test group (chat: group). It says when the tester must forward it.
//   --self-test: the dispatch on made-up updates against a fake API, no network.
//
// The method and field names are the layer-233 spec's (docs/tgw-format.md: sendWidget, answerWidgetQuery,
// editMessageWidgetData, editWidgetData, the widget_query update with id, from, message, inline_message_id,
// widget_id, data). The shape of an inline result that sends a widget is not in the spec yet: answerInline()
// assumes one; adjust it to the Bot API once it ships. Not run against a server yet.
//
// Config: the same file as publish-channel.ts, ~/.config/widget-sandbox/test-bot.env (or --config): TW_BOT_TOKEN,
// TW_BOT_USER (the tester's private chat), TW_BOT_GROUP (the test group, for chat: group), optional TW_BOT_API and
// TW_BOT_TEST_ENV=1. Never inside a git working tree.
//
//   corepack pnpm tsx scripts/conformance/test-bot.ts --self-test
//   corepack pnpm tsx scripts/conformance/test-bot.ts --post visual/sdk/20-forward-requests --variant live-copy
//   corepack pnpm tsx scripts/conformance/test-bot.ts

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import type { BotAction, MessageSpec } from './lib';

const ROOT = path.join(path.dirname(new URL(import.meta.url).pathname), '..', '..');
const CONF = path.join(ROOT, 'packages', 'conformance');
const args = process.argv.slice(2);
const opt = (name: string) => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined);
const die = (m: string): never => { console.error(`test-bot: ${m}`); process.exit(1); };

interface Post { widget: string; width: number; height: number; message?: MessageSpec; variant?: string }
interface Channel { posts: Post[]; bot: Record<string, { case: string; action: BotAction }> }
const channel = (): Channel => JSON.parse(fs.readFileSync(path.join(CONF, 'channel.json'), 'utf8'));

// ---- the Bot API

type Api = (method: string, params: Record<string, unknown>) => Promise<any>;

function loadConfig() {
  const file = opt('--config') ?? path.join(os.homedir(), '.config', 'widget-sandbox', 'test-bot.env');
  const vals: Record<string, string> = {};
  if (fs.existsSync(file)) {
    for (let d = path.dirname(path.resolve(file)); path.dirname(d) !== d; d = path.dirname(d)) if (fs.existsSync(path.join(d, '.git'))) die(`the config ${file} is inside a git working tree`);
    for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
      const m = /^\s*(?:export\s+)?([A-Z0-9_]+)\s*=\s*(.*?)\s*$/.exec(line);
      if (m && !line.trim().startsWith('#')) vals[m[1]] = m[2].replace(/^(['"])(.*)\1$/, '$2');
    }
  }
  const get = (k: string) => process.env[k] || vals[k] || '';
  const token = get('TW_BOT_TOKEN') || die(`no TW_BOT_TOKEN (${file})`);
  const base = (get('TW_BOT_API') || 'https://api.telegram.org').replace(/\/+$/, '');
  const group = get('TW_BOT_GROUP');
  return {
    user: get('TW_BOT_USER'),
    group: group && !group.startsWith('-100') ? `-100${group}` : group,
    base: get('TW_BOT_TEST_ENV') === '1' ? `${base}/bot${token}/test` : `${base}/bot${token}`,
  };
}

function botApi(base: string): Api {
  return async (method, params) => {
    const res = await fetch(`${base}/${method}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(params) });
    const body = (await res.json()) as { ok: boolean; result?: unknown; description?: string; parameters?: { retry_after?: number } };
    if (body.ok) return body.result;
    if (body.parameters?.retry_after) { await new Promise((r) => setTimeout(r, body.parameters!.retry_after! * 1000)); return botApi(base)(method, params); }
    throw new Error(`${method}: ${body.description}`);
  };
}

// ---- answering widget queries

const counters = new Map<string, number>();
const fill = (s: string, n: number, from: string) =>
  s.replace(/\{n\}/g, String(n)).replace(/\{from\}/g, from).replace(/\{time\}/g, new Date().toTimeString().slice(0, 8));

/** One widget_query (Bot API): what the table says for its data, then the answer. */
export async function handleQuery(api: Api, q: any, table: Channel['bot'], schedule = setTimeout as (f: () => void, ms: number) => unknown) {
  const action = table[q.data]?.action;
  if (!action) return api('answerWidgetQuery', { widget_query_id: q.id });
  // A copy has no message: its counter (and its edits) go by the shared widget.
  const where = q.message ? `${q.message.chat.id}:${q.message.message_id}` : q.inline_message_id ?? `widget:${q.widget_id}`;
  const key = `${where}|${q.data}`;
  const from = q.from?.first_name ?? String(q.from?.id ?? '');
  const edit = async () => {
    const n = (counters.get(key) ?? 0) + 1;
    counters.set(key, n);
    if (action.edit) {
      const fields = Object.fromEntries(Object.entries(action.edit).map(([k, v]) => [k, fill(v as string, n, from)]));
      if (q.message) await api('editMessageWidgetData', { chat_id: q.message.chat.id, message_id: q.message.message_id, ...fields });
      else if (q.inline_message_id) await api('editMessageWidgetData', { inline_message_id: q.inline_message_id, ...fields });
      // A copy: only the shared state, by the widget's id (a copy has no chat state).
      else if (q.widget_id && fields.public_data !== undefined) await api('editWidgetData', { widget_id: q.widget_id, public_data: fields.public_data });
    }
    if (action.editShared && q.widget_id) {
      await api('editWidgetData', { widget_id: q.widget_id, public_data: fill(action.editShared.public_data, n, from), ...(action.editShared.cache_time !== undefined ? { cache_time: action.editShared.cache_time } : {}) });
    }
    return n;
  };
  const n = action.edit || action.editShared ? await edit() : (counters.set(key, (counters.get(key) ?? 0) + 1), counters.get(key)!);
  if (action.repeat) for (let i = 1; i < action.repeat.times; i++) schedule(() => { edit().catch((e) => console.error(`repeat ${q.data}: ${e.message}`)); }, i * action.repeat.every * 1000);
  return api('answerWidgetQuery', { widget_query_id: q.id, ...(action.result !== undefined ? { result: fill(action.result, n, from) } : {}) });
}

// ---- sending

const sendFields = (m: MessageSpec | undefined, p: Post) => ({
  html: fs.readFileSync(path.join(CONF, p.widget), 'utf8'),
  width: p.width, height: p.height,
  ...(m?.allow_queries ? { allow_queries: true } : {}),
  ...(m?.fwd_allow_queries ? { fwd_allow_queries: true } : {}),
  ...(m?.shared_widget ? { shared_widget: true } : {}),
  ...(m?.chat_data !== undefined ? { chat_data: m.chat_data } : {}),
  ...(m?.public_data !== undefined ? { public_data: m.public_data } : {}),
});

/** The inline query conf-preview: the preview cases as widget results (the result's shape is assumed: see the top). */
async function answerInline(api: Api, q: any, posts: Post[]) {
  const results = posts.filter((p) => p.message?.inline).map((p, i) => ({ type: 'widget', id: String(i + 1), title: p.widget, ...sendFields(p.message, p) }));
  return api('answerInlineQuery', { inline_query_id: q.id, results, cache_time: 0, is_personal: true });
}

async function post(api: Api, cfg: ReturnType<typeof loadConfig>, id: string, variant?: string) {
  const posts = channel().posts.filter((p) => p.widget === `${id}.html` && (!variant || p.variant === variant));
  if (!posts.length) die(`no post ${id}${variant ? ` (${variant})` : ''} in channel.json`);
  for (const p of posts) {
    const m = p.message;
    if (m?.inline) { console.log(`${id}: an inline result: type the bot's username and conf-preview in any chat (the bot must be running)`); continue; }
    const chat = m?.chat === 'group' ? cfg.group || die('TW_BOT_GROUP is not set') : cfg.user || die('TW_BOT_USER is not set');
    const sent = await api('sendWidget', { chat_id: chat, ...sendFields(m, p) });
    console.log(`${id}${p.variant ? ` (${p.variant})` : ''}: message ${sent?.message_id} in ${chat}${m?.forward ? ': now forward it, the test looks at the forward' : ''}`);
  }
}

async function run(api: Api) {
  const { bot, posts } = channel();
  console.log(`test-bot: ${Object.keys(bot).length} request data in the table; answering until Ctrl-C`);
  let offset = 0;
  for (;;) {
    let updates: any[] = [];
    try { updates = await api('getUpdates', { offset, timeout: 30, allowed_updates: ['widget_query', 'inline_query'] }); } catch (e) { console.error((e as Error).message); await new Promise((r) => setTimeout(r, 3000)); continue; }
    for (const u of updates) {
      offset = u.update_id + 1;
      try {
        if (u.widget_query) { console.log(`widget_query ${JSON.stringify(u.widget_query.data)} from ${u.widget_query.from?.first_name}`); await handleQuery(api, u.widget_query, bot); }
        else if (u.inline_query && u.inline_query.query.trim() === 'conf-preview') await answerInline(api, u.inline_query, posts);
      } catch (e) { console.error((e as Error).message); }
    }
  }
}

// ---- self-test: the dispatch on made-up updates, no network

async function selfTest() {
  const { bot } = channel();
  const calls: [string, any][] = [];
  const api: Api = async (method, params) => { calls.push([method, params]); return true; };
  const scheduled: number[] = [];
  const from = { id: 7, first_name: 'Ann' };
  const expect = (want: [string, any][], what: string) => {
    const got = JSON.stringify(calls.splice(0));
    if (got !== JSON.stringify(want)) die(`${what}: got ${got}, want ${JSON.stringify(want)}`);
    console.log(`ok ${what}`);
  };
  const msg = { message_id: 42, chat: { id: 1001, type: 'private' } };
  await handleQuery(api, { id: 'q1', from, message: msg, data: 'conf:16:edit' }, bot);
  await handleQuery(api, { id: 'q2', from, message: msg, data: 'conf:16:edit' }, bot);
  expect([
    ['editMessageWidgetData', { chat_id: 1001, message_id: 42, chat_data: '1', public_data: '1' }], ['answerWidgetQuery', { widget_query_id: 'q1', result: 'edited 1' }],
    ['editMessageWidgetData', { chat_id: 1001, message_id: 42, chat_data: '2', public_data: '2' }], ['answerWidgetQuery', { widget_query_id: 'q2', result: 'edited 2' }],
  ], 'a state edit by the message, counted per message (16-state-edit)');
  await handleQuery(api, { id: 'q3', from, widget_id: '77', data: 'conf:20:ask' }, bot);
  expect([['answerWidgetQuery', { widget_query_id: 'q3', result: 'answered for Ann' }]], 'a copy: only from, data and widget_id (20-forward-requests)');
  await handleQuery(api, { id: 'q4', from, message: msg, widget_id: '88', data: 'conf:24:start' }, bot, (_f, ms) => scheduled.push(ms));
  expect([['editWidgetData', { widget_id: '88', public_data: '1', cache_time: 5 }], ['answerWidgetQuery', { widget_query_id: 'q4', result: 'ticking' }]], 'a shared edit by widget_id, then the answer (24-watch-public-data)');
  if (scheduled.length !== 11 || scheduled[0] !== 10_000) die(`the ticker: got ${scheduled.length} repeats, first at ${scheduled[0]} ms`);
  console.log('ok the ticker repeats 11 more times, every 10 s');
  await handleQuery(api, { id: 'q5', from, message: { message_id: 5, chat: { id: -100123, type: 'supergroup' } }, data: 'conf:25:plus' }, bot);
  expect([['editMessageWidgetData', { chat_id: -100123, message_id: 5, chat_data: '{"n":1,"by":"Ann"}' }], ['answerWidgetQuery', { widget_query_id: 'q5', result: 'ok' }]], 'a group\'s chat state (25-group-chat-state)');
  await handleQuery(api, { id: 'q6', from, message: msg, data: 'conf:nope' }, bot);
  expect([['answerWidgetQuery', { widget_query_id: 'q6' }]], 'unknown data: an empty answer');
  console.log(`test-bot self-test: ${Object.keys(bot).length} request data in the table, all fine`);
}

async function main() {
  if (args.includes('--self-test')) return selfTest();
  const cfg = loadConfig();
  const api = botApi(cfg.base);
  const id = opt('--post');
  if (id) return post(api, cfg, id.replace(/\.html$/, ''), opt('--variant'));
  return run(api);
}

main().catch((e) => die((e as Error).message));
