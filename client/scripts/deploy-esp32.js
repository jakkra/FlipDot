#!/usr/bin/env node
import { promises as fsp } from 'fs';
import { createReadStream, createWriteStream } from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createGzip } from 'zlib';
import { pipeline } from 'stream/promises';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const clientRoot = path.resolve(__dirname, '..');
const distDir = path.join(clientRoot, 'dist');
const firmwareRoot = path.resolve(clientRoot, '..', 'main', 'web_static');
const firmwareAssetsDir = path.join(firmwareRoot, 'assets');

async function ensureBuildArtifacts() {
  const required = [
    path.join(distDir, 'index.html'),
    path.join(distDir, 'assets'),
    path.join(distDir, 'vite.svg'),
  ];
  await Promise.all(required.map(async (filePath) => {
    try {
      await fsp.access(filePath);
    } catch (error) {
      throw new Error(`Missing build artifact: ${filePath}. Run \`npm run build\` first.`);
    }
  }));
}

async function selectBundle() {
  const assetsDir = path.join(distDir, 'assets');
  const entries = await fsp.readdir(assetsDir);
  const jsBundle = entries.find((entry) => entry.endsWith('.js'));
  if (!jsBundle) {
    throw new Error('No JavaScript bundle found in dist/assets');
  }
  return path.join(assetsDir, jsBundle);
}

async function copyFile(src, dest) {
  await fsp.copyFile(src, dest);
  console.log(`Copied ${path.relative(clientRoot, src)} -> ${path.relative(clientRoot, dest)}`);
}

async function copyAndFixHtml(src, dest, originalBundleName) {
  let htmlContent = await fsp.readFile(src, 'utf8');
  // Replace the hashed bundle name with 'app.js'
  const bundleBasename = path.basename(originalBundleName);
  htmlContent = htmlContent.replace(`/assets/${bundleBasename}`, '/assets/app.js');
  await fsp.writeFile(dest, htmlContent);
  console.log(`Copied and fixed ${path.relative(clientRoot, src)} -> ${path.relative(clientRoot, dest)}`);
}

async function gzipFile(src, dest) {
  await pipeline(createReadStream(src), createGzip({ level: 9 }), createWriteStream(dest));
  console.log(`Compressed ${path.relative(clientRoot, src)} -> ${path.relative(clientRoot, dest)}`);
}

async function main() {
  await ensureBuildArtifacts();
  await fsp.mkdir(firmwareAssetsDir, { recursive: true });

  const indexSrc = path.join(distDir, 'index.html');
  const svgSrc = path.join(distDir, 'vite.svg');
  const bundleSrc = await selectBundle();

  const indexDest = path.join(firmwareRoot, 'index.html');
  const svgDest = path.join(firmwareRoot, 'vite.svg');
  const bundleDest = path.join(firmwareAssetsDir, 'app.js');

  await copyAndFixHtml(indexSrc, indexDest, bundleSrc);
  await copyFile(svgSrc, svgDest);
  await copyFile(bundleSrc, bundleDest);

  await gzipFile(indexDest, `${indexDest}.gz`);
  await gzipFile(svgDest, `${svgDest}.gz`);
  await gzipFile(bundleDest, `${bundleDest}.gz`);

  console.log('Static assets prepared under main/web_static. Rebuild the firmware to flash the updated UI.');
}

main().catch((error) => {
  console.error(error.message || error);
  process.exit(1);
});
