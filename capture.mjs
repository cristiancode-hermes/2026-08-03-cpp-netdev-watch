import puppeteer from 'puppeteer';
import { mkdirSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

async function captureNetdevWatch() {
  const htmlPath = '/opt/data/cpp-projects/2026-08-03-cpp-netdev-watch/screenshot.html';
  const assetsDir = '/opt/data/portafolio/public/assets';
  mkdirSync(assetsDir, { recursive: true });

  const browser = await puppeteer.launch({
    executablePath: '/opt/data/.agent-browser/browsers/chromium/chrome-linux/chrome',
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage'],
  });

  try {
    // Desktop capture — grow viewport if terminal is taller than 900px
    const page = await browser.newPage();
    let viewportH = 900;
    await page.setViewport({ width: 1440, height: viewportH });
    await page.goto('file://' + htmlPath, { waitUntil: 'networkidle0', timeout: 15000 });
    await page.waitForSelector('.terminal', { timeout: 5000 });
    await new Promise(r => setTimeout(r, 1000));

    let box = await (await page.$('.terminal')).boundingBox();
    if (box.height + 60 > viewportH) {
      viewportH = Math.ceil(box.height + 140);
      await page.setViewport({ width: 1440, height: viewportH });
      await new Promise(r => setTimeout(r, 500));
      box = await (await page.$('.terminal')).boundingBox();
    }

    await page.screenshot({
      path: join(assetsDir, 'netdev-watch.png'),
      clip: {
        x: Math.max(0, box.x - 40),
        y: Math.max(0, box.y - 20),
        width: Math.min(box.width + 80, 1440),
        height: Math.min(box.height + 40, viewportH),
      },
    });

    // Mobile capture
    await page.setViewport({ width: 390, height: 844 });
    await page.goto('file://' + htmlPath, { waitUntil: 'networkidle0', timeout: 15000 });
    await page.waitForSelector('.terminal', { timeout: 5000 });
    await new Promise(r => setTimeout(r, 1000));

    const box2 = await (await page.$('.terminal')).boundingBox();

    await page.screenshot({
      path: join(assetsDir, 'netdev-watch-m.png'),
      clip: {
        x: Math.max(0, box2.x - 20),
        y: Math.max(0, box2.y - 10),
        width: Math.min(box2.width + 40, 390),
        height: Math.min(box2.height + 20, 844),
      },
    });

    console.log('Screenshots captured successfully');
    console.log(`Desktop: ${join(assetsDir, 'netdev-watch.png')}`);
    console.log(`Mobile: ${join(assetsDir, 'netdev-watch-m.png')}`);
  } finally {
    await browser.close();
  }
}

captureNetdevWatch().catch(err => {
  console.error('Capture failed:', err);
  process.exit(1);
});
