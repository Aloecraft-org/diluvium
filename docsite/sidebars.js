// sidebars.js -- the order the documentation is read in.
//
// ── entry points ───────────────────────────────────────────────────────
//   `docs`  the one sidebar; docusaurus.config.js names it in the navbar.
//
// ── fan-out ────────────────────────────────────────────────────────────
//   Every id below is a file in doc/, without the extension, and every
//   one of them must also be in PUBLISHED in docusaurus.config.js -- that
//   list decides what EXISTS, this one decides what ORDER it is read in.
//   An id here that is not published fails the build, which is the point:
//   the two lists disagreeing is the bug, not something to discover as a
//   missing sidebar entry on the live site.

/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
module.exports = {
  docs: [
    { type: 'doc', id: 'README', label: 'Overview' },
    {
      type: 'category',
      label: 'The language',
      collapsed: false,
      items: [
        'Guide',
        'diluvium-numeric-spec',
        'Determinism',
        'Analyzer',
      ],
    },
    {
      type: 'category',
      label: 'Embedding and extending',
      items: [
        'Host',
        'Hostcall',
        'Extending',
        'Capabilities',
        'Hibernate',
      ],
    },
    {
      type: 'category',
      label: 'DRT, the runtime',
      items: [
        'DRT',
        'Messaging',
      ],
    },
    {
      type: 'category',
      label: 'Around the language',
      items: [
        'Lab',
        'Benchmarks',
      ],
    },
  ],
};
