# The Rig website

The source of https://woozchucky.github.io/rig/, the language's specification as a website, built with [VitePress](https://vitepress.dev/).

The content comes from [`docs/rig-spec.md`](../docs/rig-spec.md), the specification, and nothing else: `scripts/spec.mjs` splits it into a page per section, rewrites its links for the site and labels its examples, before every build. The generated pages (`spec/`, `.vitepress/generated/`) are not committed; to change the specification, edit `docs/rig-spec.md`. A link in it to a heading, a section or a repository file that does not exist fails the build. Rig's code is highlighted with the VS Code extension's grammar, `editors/vscode/syntaxes/rig.tmLanguage.json`.

Node.js is needed only for the site, never for building the compiler. To preview it, from this directory:

```sh
npm ci && npm run docs:dev
```

The dev server regenerates the pages when `docs/rig-spec.md` changes. `npm run docs:build` builds the site into `.vitepress/dist`, and `npm run docs:preview` serves that build.

The [docs workflow](../.github/workflows/docs.yml) builds the site for every pull request that touches it, and publishes it to GitHub Pages from `main`.
