// The stylesheet of a reference rendering: the widget file drawn natively by a
// browser engine, without the sandbox (the dev app's ?reference=1, the native
// demos' reference mode, the conformance set's Chromium runs). Dev tooling,
// never a product path. It carries what the renderer itself adds to every
// widget: the UA layer of the profile (profile.json `uaStylesheet`) and the
// theme variables, here with the default palettes and the reference text
// tokens (DEFAULT_TEXT_TOKENS), so a reference picture shows what the profile
// removes, not a different root style.

import { PROFILE_UA_STYLESHEET } from './profile-tables.ts';
import { themeVariables } from './sdk-policy.ts';

const rootVars = (scheme: 'light' | 'dark') =>
  Object.entries(themeVariables({ colorScheme: scheme })).map(([k, v]) => `${k}:${v}`).join(';');

/**
 * The UA layer, then the reference theme on `:root` for both colour schemes (`prefers-color-scheme` picks one). Not
 * `!important` (the renderer's are): a reference harness that sets its own palette inline still wins.
 */
export const REFERENCE_STYLESHEET =
  `${PROFILE_UA_STYLESHEET}:root{${rootVars('light')}}@media (prefers-color-scheme: dark){:root{${rootVars('dark')}}}`;
