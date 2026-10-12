// Element / attribute allowlists (security-critical, invariant 2).
// Anything not listed here does not reach the renderer.

const words = (s: string) => s.trim().split(/\s+/);

/** HTML elements rendered as-is. */
export const HTML_TAGS = new Set(words(`
  html head body div span p a b i u s em strong small big sub sup mark code pre kbd samp var abbr cite q blockquote
  del ins dfn time data address br hr wbr
  h1 h2 h3 h4 h5 h6 header footer main nav section article aside hgroup search
  ul ol li dl dt dd menu
  table thead tbody tfoot tr td th caption colgroup col
  form input button select option optgroup textarea label fieldset legend datalist output progress meter
  details summary dialog figure figcaption picture img canvas
  ruby rt rp bdi bdo center font tt strike nobr
`));

/** Canonical (case-sensitive) names of allowed SVG elements, keyed by lowercase. */
export const SVG_TAGS = new Map(
  words(`
  svg g path rect circle ellipse line polyline polygon text tspan textPath defs symbol use image clipPath mask
  pattern marker linearGradient radialGradient stop filter foreignObject title desc a switch view
  feBlend feColorMatrix feComponentTransfer feComposite feConvolveMatrix feDiffuseLighting feDisplacementMap
  feDistantLight feDropShadow feFlood feFuncA feFuncB feFuncG feFuncR feGaussianBlur feImage feMerge feMergeNode
  feMorphology feOffset fePointLight feSpecularLighting feSpotLight feTile feTurbulence
  animate animateTransform animateMotion mpath
`).map((t) => [t.toLowerCase(), t]),
);

/**
 * Elements that are never rendered, together with their whole subtree.
 * (`style` is not here: it is handled separately through the CSS sanitizer.)
 * `audio`/`video`/`source`/`track` are dropped on purpose: a chat bubble has no
 * use for media playback, and `data:` media would hand the platform's codec
 * stack to the widget (and autoplay / battery). `<picture>` stays as a plain
 * wrapper around its `<img>`.
 */
export const DROPPED_TAGS = new Set(words(`
  script iframe frame frameset object embed link meta base portal noscript set title template applet param
  noembed noframes plaintext xmp listing slot track area map math metadata fencedframe audio video source
`));

/** Attributes allowed on any element (after lowercasing for HTML). */
const GLOBAL_ATTRS = words(`
  class id title lang dir hidden tabindex role draggable contenteditable spellcheck translate inert popover
  enterkeyhint inputmode autocapitalize slot part name
  type value checked disabled readonly required placeholder min max step minlength maxlength pattern multiple
  selected size rows cols wrap for label accept autocomplete list open colspan rowspan headers scope span
  width height alt align valign bgcolor border cellpadding cellspacing color face nowrap start reversed
  low high optimum datetime novalidate method enctype
  dirname popovertarget popovertargetaction command commandfor
`);

/** SVG attributes (canonical case). Presentation + geometry + filter + animation. */
const SVG_ATTRS = words(`
  viewBox preserveAspectRatio x y x1 y1 x2 y2 cx cy r rx ry d points dx dy rotate transform
  width height fill fill-opacity fill-rule stroke stroke-width stroke-opacity stroke-linecap stroke-linejoin
  stroke-dasharray stroke-dashoffset stroke-miterlimit opacity color display visibility overflow
  clip-path clip-rule mask filter marker marker-start marker-mid marker-end markerWidth markerHeight markerUnits
  refX refY orient text-anchor dominant-baseline alignment-baseline baseline-shift font-family font-size
  font-weight font-style font-variant font-stretch letter-spacing word-spacing text-decoration writing-mode
  textLength lengthAdjust startOffset method spacing side
  gradientUnits gradientTransform spreadMethod fx fy fr offset stop-color stop-opacity
  patternUnits patternContentUnits patternTransform clipPathUnits maskUnits maskContentUnits
  pointer-events shape-rendering text-rendering image-rendering color-interpolation color-interpolation-filters
  vector-effect paint-order mix-blend-mode isolation cursor pathLength
  in in2 result mode operator k1 k2 k3 k4 stdDeviation edgeMode kernelMatrix kernelUnitLength order divisor bias
  targetX targetY preserveAlpha surfaceScale diffuseConstant specularConstant specularExponent
  lighting-color flood-color flood-opacity azimuth elevation z pointsAtX pointsAtY pointsAtZ limitingConeAngle
  baseFrequency numOctaves seed stitchTiles scale xChannelSelector yChannelSelector radius values tableValues
  slope intercept amplitude exponent filterUnits primitiveUnits
  attributeName attributeType begin dur end repeatCount repeatDur from to by keyTimes keySplines keyPoints
  calcMode additive accumulate restart path
  version xml:space xml:lang systemLanguage requiredExtensions
`);

export const ALLOWED_ATTRS = new Set([...GLOBAL_ATTRS, ...SVG_ATTRS.map((a) => a.toLowerCase())]);

/** lowercase → canonical case for SVG attributes (HTML-parser style adjustment). */
export const SVG_ATTR_CASE = new Map(SVG_ATTRS.filter((a) => a !== a.toLowerCase()).map((a) => [a.toLowerCase(), a]));

/**
 * Every attribute that can carry a URL. None of these is ever passed through
 * unchecked; unlisted (tag, attr) combinations are dropped. See validator.ts.
 */
export const URL_ATTRS = new Set(words(`
  href xlink:href src srcset poster action formaction background ping data cite codebase manifest longdesc
  usemap profile icon archive classid lowsrc dynsrc srcdoc imagesrcset imagesizes attributionsrc
`));

/** Attributes whose value is CSS and may contain url(): parsed by the CSS sanitizer. */
export const CSS_VALUE_ATTRS = new Set(words(`
  fill stroke filter mask clip-path marker marker-start marker-mid marker-end cursor color flood-color
  lighting-color stop-color
`));

/** Attribute names an SVG animation element may target. URL-like ones are excluded. */
export const ANIMATION_VALUE_ATTRS = new Set(['from', 'to', 'by', 'values']);

/** Attributes dropped silently (noise, not worth a diagnostic). */
export const SILENT_DROP_ATTRS = new Set(words(`
  xmlns xmlns:xlink target rel download autofocus nonce is loading decoding crossorigin referrerpolicy
  integrity fetchpriority charset http-equiv content async defer
`));

/** Properties settable through the `prop` op, with the tags they apply to (null = any). */
export const PROPS: Record<string, { type: 'string' | 'number' | 'boolean'; tags: Set<string> | null }> = {
  value: { type: 'string', tags: new Set(words('input textarea select option progress meter button output')) },
  checked: { type: 'boolean', tags: new Set(['input']) },
  indeterminate: { type: 'boolean', tags: new Set(['input']) },
  selectedIndex: { type: 'number', tags: new Set(['select']) },
  selected: { type: 'boolean', tags: new Set(['option']) },
  scrollTop: { type: 'number', tags: null },
  scrollLeft: { type: 'number', tags: null },
};

/** Methods callable through the `call` op: name → number of primitive args forwarded. */
export const METHODS: Record<string, number> = {
  focus: 0, blur: 0, select: 0, scrollIntoView: 0, setPointerCapture: 1, releasePointerCapture: 1,
  showModal: 0, show: 0, close: 0, reset: 0, scrollTo: 2, scrollBy: 2, setSelectionRange: 2,
  showPopover: 0, hidePopover: 0, togglePopover: 0,
  // Not DOM methods: (un)subscribe the element in the renderer's ResizeObserver.
  observeResize: 0, unobserveResize: 0,
};

/** Canvas 2D: methods that may be replayed. Nothing here takes a URL. */
export const CANVAS_METHODS = new Set(words(`
  save restore scale rotate translate transform setTransform resetTransform
  clearRect fillRect strokeRect beginPath closePath moveTo lineTo bezierCurveTo quadraticCurveTo arc arcTo
  ellipse rect roundRect fill stroke clip fillText strokeText setLineDash drawImage putImageData reset
`));

/** Canvas 2D: settable properties. `filter` is additionally CSS-sanitized. */
export const CANVAS_PROPS = new Set(words(`
  fillStyle strokeStyle lineWidth lineCap lineJoin miterLimit lineDashOffset font textAlign textBaseline
  direction globalAlpha globalCompositeOperation shadowBlur shadowColor shadowOffsetX shadowOffsetY
  imageSmoothingEnabled imageSmoothingQuality filter letterSpacing wordSpacing fontKerning fontStretch
  fontVariantCaps textRendering
`));

export const CANVAS_PATH_METHODS = new Set(words(`
  moveTo lineTo bezierCurveTo quadraticCurveTo arc arcTo ellipse rect roundRect closePath addPath
`));

export const SVG_QUERY_METHODS = new Set(words(`
  getBBox getComputedTextLength getTotalLength getPointAtLength getCTM getScreenCTM getSubStringLength
  getNumberOfChars
`));

export const CANVAS_QUERY_METHODS = new Set(words(`isPointInPath isPointInStroke getTransform getLineDash toDataURL`));
