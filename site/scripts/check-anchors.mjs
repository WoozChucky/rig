// Checks the built site (.vitepress/dist) for links to anchors that don't
// exist: every href of a page that names one of the site's own pages and a
// '#fragment' must name an id on that page. Run after `vitepress build`; it
// prints each dangling link and exits with status 1 if there is one.
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const siteDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dist = path.join(siteDir, '.vitepress', 'dist')
// The site is served under this path (config.mts's `base`).
const base = '/rig/'

function htmlFiles(dir) {
  return readdirSync(dir).flatMap((name) => {
    const file = path.join(dir, name)
    if (statSync(file).isDirectory()) return htmlFiles(file)
    return name.endsWith('.html') ? [file] : []
  })
}

// Where the site is published; an absolute link to it is checked like a
// relative one.
const siteOrigin = 'https://woozchucky.github.io'

// The built file a site path is served from, with clean URLs:
// /rig/spec/types -> spec/types.html, /rig/spec/ and /rig/spec -> spec/index.html.
function fileFor(sitePath) {
  let rel = decodeURI(sitePath.slice(base.length)).replace(/\.html$/, '')
  if (rel === '' || rel.endsWith('/')) rel += 'index'
  const file = path.join(dist, `${rel}.html`)
  const index = path.join(dist, rel, 'index.html')
  return !existsSync(file) && existsSync(index) ? index : file
}

// The site path a built file is served at: spec/types.html -> /rig/spec/types,
// spec/index.html -> /rig/spec/. Relative links are resolved against it.
function urlFor(file) {
  const rel = path.relative(dist, file).split(path.sep).join('/').replace(/\.html$/, '')
  if (rel === 'index') return base
  if (rel.endsWith('/index')) return base + rel.slice(0, -'index'.length)
  return base + rel
}

const idsCache = new Map()
function idsOf(file) {
  if (!idsCache.has(file)) {
    const html = readFileSync(file, 'utf8')
    idsCache.set(file, new Set([...html.matchAll(/\sid="([^"]*)"/g)].map((m) => m[1])))
  }
  return idsCache.get(file)
}

if (!existsSync(dist)) {
  console.error(`check-anchors: ${path.relative(siteDir, dist)} doesn't exist; run vitepress build first`)
  process.exit(1)
}

const dangling = []
let checked = 0
for (const file of htmlFiles(dist)) {
  const html = readFileSync(file, 'utf8')
  const page = '/' + path.relative(dist, file).split(path.sep).join('/')
  const pageUrl = new URL(urlFor(file), siteOrigin)
  for (const [, raw] of html.matchAll(/\shref="([^"]*#[^"]*)"/g)) {
    // The href as the browser reads it: entities decoded, then resolved
    // against the page, so relative links and '?query' are handled too.
    const href = raw.replace(/&amp;/g, '&')
    let url
    try {
      url = new URL(href, pageUrl)
    } catch {
      dangling.push(`${page}: ${href}: not a valid URL`)
      continue
    }
    const fragment = url.hash.slice(1)
    if (fragment === '' || fragment.startsWith(':~:')) continue
    if (url.origin !== siteOrigin) continue // another site
    checked++
    if (!url.pathname.startsWith(base)) {
      dangling.push(`${page}: ${href}: outside the site, which is served under ${base}`)
      continue
    }
    const targetFile = fileFor(url.pathname)
    const id = decodeURIComponent(fragment)
    if (!existsSync(targetFile)) dangling.push(`${page}: ${href}: no page ${path.relative(dist, targetFile)}`)
    else if (!idsOf(targetFile).has(id)) dangling.push(`${page}: ${href}: no id "${id}" on that page`)
  }
}

for (const d of [...new Set(dangling)]) console.error(`check-anchors: ${d}`)
if (dangling.length > 0) {
  console.error(`check-anchors: ${dangling.length} of ${checked} links to an anchor point at nothing`)
  process.exit(1)
}
console.log(`check-anchors: ${checked} links to an anchor, all found`)
