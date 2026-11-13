import { readFileSync } from 'node:fs'
import path from 'node:path'
import { defineConfig, type Plugin } from 'vitepress'
import { generate, repoDir, repoUrl, specPath } from '../scripts/spec.mjs'
// Written by scripts/spec.mjs from the specification's headings. Importing it
// makes it one of the config's dependencies, so the dev server restarts with
// the new sidebar when a heading changes.
import sidebar from './generated/sidebar.json' with { type: 'json' }

// Rig's highlighting is the VS Code extension's own grammar.
const rigGrammar = JSON.parse(
  readFileSync(path.join(repoDir, 'editors', 'vscode', 'syntaxes', 'rig.tmLanguage.json'), 'utf8')
)

// In `vitepress dev`, regenerates the pages when docs/rig-spec.md changes.
function specWatcher(): Plugin {
  return {
    name: 'rig-spec',
    apply: 'serve',
    configureServer(server) {
      // The directory, not the file: a file replaced by a new one (as git
      // checkout does) is no longer watched, but its directory sees it added.
      server.watcher.add(path.dirname(specPath))
      const regenerate = (file: string) => {
        if (path.resolve(file) !== path.resolve(specPath)) return
        try {
          generate()
          server.config.logger.info('spec: pages regenerated from docs/rig-spec.md', { timestamp: true })
        } catch (e) {
          server.config.logger.error(`spec: ${(e as Error).message}`, { timestamp: true })
        }
      }
      server.watcher.on('change', regenerate)
      server.watcher.on('add', regenerate)
    }
  }
}

export default defineConfig({
  title: 'Rig',
  description: 'Rig, a compiled systems programming language for Windows and Linux, in the spirit of Jai',
  lang: 'en-GB',
  base: '/rig/',
  cleanUrls: true,
  lastUpdated: true,
  // The site's own README is for the repository, not a page.
  srcExclude: ['README.md'],
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: '/rig/logo.svg' }]],

  markdown: {
    languages: [
      { ...rigGrammar, name: 'rig', displayName: 'Rig' },
      // Shiki has no EBNF grammar: Appendix A's productions are shown plain,
      // under their own name.
      { name: 'ebnf', displayName: 'EBNF', scopeName: 'source.ebnf', patterns: [], repository: {} }
    ]
  },

  vite: {
    plugins: [specWatcher()]
  },

  themeConfig: {
    logo: { light: '/logo.svg', dark: '/logo-dark.svg', alt: '' },
    nav: [
      { text: 'Specification', link: '/spec/', activeMatch: '^/spec/' },
      { text: 'Roadmap', link: `${repoUrl}/issues/85` }
    ],
    sidebar: { '/spec/': sidebar },
    outline: { level: [2, 3] },
    search: { provider: 'local' },
    editLink: {
      // Every page of the specification is made from docs/rig-spec.md. The
      // function is sent to the browser on its own, so it names the
      // repository itself rather than through repoUrl.
      pattern: ({ filePath }) =>
        filePath.startsWith('spec/')
          ? 'https://github.com/WoozChucky/rig/edit/main/docs/rig-spec.md'
          : `https://github.com/WoozChucky/rig/edit/main/site/${filePath}`,
      text: 'Edit this page on GitHub'
    },
    socialLinks: [{ icon: 'github', link: repoUrl }],
    footer: { message: 'Released under the MIT License.' }
  }
})
