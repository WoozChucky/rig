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

// The built file a site path is served from, with clean URLs:
// /rig/spec/types -> spec/types.html, /rig/spec/ -> spec/index.html.
function fileFor(sitePath) {
  let rel = decodeURI(sitePath.slice(base.length))
  if (rel === '' || rel.endsWith('/')) rel += 'index'
  rel = rel.replace(/\.html$/, '')
  return path.join(dist, `${rel}.html`)
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
  for (const [, href] of html.matchAll(/\shref="([^"]*#[^"]*)"/g)) {
    const [target, fragment] = href.split('#', 2)
    if (fragment === '' || fragment.startsWith(':~:')) continue
    let targetFile
    if (target === '') targetFile = file
    else if (target.startsWith(base)) targetFile = fileFor(target)
    else continue // another site, or a path outside the site
    checked++
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
