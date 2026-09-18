// docusaurus.config.js -- diluvium.aloecraft.org/docs.
//
// ── entry points ───────────────────────────────────────────────────────
//   `npm run build` in this directory -> build/, which
//   deploy/cloud1/docs/deploy.sh ships to cloud1. Nothing else reads this
//   file, and site/build.sh never does: that build is the Aloecraft site
//   contract -- offline, hermetic, no npm -- and this one is an `npm ci`.
//   The two live side by side on one vhost and never call each other.
//
// ── configurable values ────────────────────────────────────────────────
//   URL / BASE_URL   where the built site believes it lives. BASE_URL is
//                    '/docs/' and the landing page at '/' is site/'s.
//   DOC_DIR          the markdown, read from the repo's own doc/ rather
//                    than copied here. One source of truth: an edit to
//                    doc/Guide.md is the docs site changing.
//   PUBLISHED        which of doc/ is public. Everything not listed is
//                    excluded -- see the comment on it, which is the
//                    whole of why this is an allowlist.
//
// ── fan-out ────────────────────────────────────────────────────────────
//   PUBLISHED        -> the pages that exist, and sidebars.js orders them
//   navbar.items     -> the header links
//   footer.links     -> the footer
//   prism            -> syntax highlighting; 'lua' is what Diluvium reads as

const PRISM = require('prism-react-renderer');

const URL = 'https://diluvium.aloecraft.org';
const BASE_URL = '/docs/';

// The markdown lives in doc/, one level up. Docusaurus is content-agnostic
// about where that is, and copying it here would make every edit two edits
// and every review a diff against a copy.
const DOC_DIR = '../doc';

// An ALLOWLIST, not a deny list. doc/ carries the roadmap, the 2026-09
// plan, the alignment audit, the BUILD* notes and audit/M0-M7 -- internal
// working documents that are fine in the repo and are not a public
// reference. A deny list publishes the next one somebody adds; this
// publishes nothing until it is named here.
const PUBLISHED = [
  'README.md',
  'Guide.md',
  'Capabilities.md',
  'Extending.md',
  'Analyzer.md',
  'Determinism.md',
  'diluvium-numeric-spec.md',
  'DRT.md',
  'Host.md',
  'Hostcall.md',
  'Hibernate.md',
  'Messaging.md',
  'Lab.md',
  'Benchmarks.md',
];

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Diluvium',
  tagline: 'Lua for Modern Development',
  url: URL,
  baseUrl: BASE_URL,
  favicon: 'favicon.ico',
  organizationName: 'Aloecraft-org',
  projectName: 'diluvium',

  // A broken link is a 404 on a published reference, which is the failure
  // this whole directory exists to avoid. doc/ cross-links files this site
  // does not publish, so those are reported and not fatal; a link to a
  // page that should exist and does not still fails the build.
  onBrokenLinks: 'throw',

  // depth: doc/ is CommonMark written years before this site existed.
  // 'detect' reads .md as CommonMark and only .mdx as MDX, so a `{name}`
  // in prose or a bare `<` stays text instead of failing the build as a
  // JSX expression. Without this, publishing existing markdown means
  // editing it for the renderer.
  markdown: {
    format: 'detect',
    // doc/ cross-links files this site does not publish (the Guide points
    // at ROADMAP.md, the BUILD notes point at each other). Those are
    // reported, not fatal: the allowlist above is deliberate, and a link
    // into it from a published page is a thing to fix in doc/, not a
    // reason a deploy cannot happen.
    hooks: { onBrokenMarkdownLinks: 'warn' },
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          path: DOC_DIR,
          include: PUBLISHED,
          routeBasePath: '/',
          sidebarPath: require.resolve('./sidebars.js'),
          editUrl: 'https://github.com/Aloecraft-org/diluvium/edit/main/doc/',
        },
        blog: false,
        theme: { customCss: require.resolve('./src/css/custom.css') },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      // Dark by default, as on the landing page, dollup and the portal.
      // respectPrefersColorScheme is false for the same reason the landing
      // page does not consult prefers-color-scheme: the ask is a dark
      // default with a toggle, not a system-following one.
      colorMode: {
        defaultMode: 'dark',
        disableSwitch: false,
        respectPrefersColorScheme: false,
      },
      navbar: {
        title: 'Diluvium',
        logo: { alt: 'Aloecraft', src: 'aloecraft_logo.svg' },
        items: [
          { type: 'docSidebar', sidebarId: 'docs', position: 'left', label: 'Docs' },
          { href: URL + '/', label: 'Try it online', position: 'right' },
          { href: URL + '/lab/', label: 'Lab', position: 'right' },
          { href: 'https://github.com/Aloecraft-org/diluvium', label: 'GitHub', position: 'right' },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Diluvium',
            items: [
              { label: 'Home', href: URL + '/' },
              { label: 'Install', href: URL + '/start' },
              { label: 'Releases', href: 'https://software.aloecraft.org/releases/diluvium/' },
            ],
          },
          {
            title: 'Aloecraft',
            items: [
              { label: 'DRT', href: 'https://software.aloecraft.org/releases/diluvium-drt/' },
              { label: 'dollup', href: 'https://dollup.aloecraft.org' },
              { label: 'Software portal', href: 'https://software.aloecraft.org' },
            ],
          },
        ],
        copyright:
          'Copyright Michael Godfrey [2026] | aloecraft.org. Apache-2.0.',
      },
      prism: {
        theme: PRISM.themes.github,
        darkTheme: PRISM.themes.vsDark,
        // Diluvium is a superset of Lua, and Prism's Lua grammar is what
        // the landing page extends too (site/static/assets/prism-diluvium.js).
        additionalLanguages: ['lua', 'bash', 'json', 'c', 'makefile'],
      },
    }),
};

module.exports = config;
