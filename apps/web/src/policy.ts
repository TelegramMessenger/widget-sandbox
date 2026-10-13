import type { SandboxPolicy } from '@telegram/widget-sandbox/shared/protocol';

const fromQuery = new URLSearchParams(location.search).get('policy');
const fromEnv = (import.meta as unknown as { env?: Record<string, string | undefined> }).env?.VITE_TW_POLICY;

export const WIDGET_POLICY: SandboxPolicy = (fromQuery ?? fromEnv) === 'compat' ? 'compat' : 'strict';
