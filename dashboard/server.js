// Pont entre le simulateur Zephyr lancé dans un pseudo-terminal et le navigateur.
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');

const EXE = path.join(os.homedir(), 'zephyrproject/build/ecu_monitor/zephyr/zephyr.exe');
const PAGE = path.join(__dirname, 'ecu_dashboard.html');
const PORT = 3000;

if (!fs.existsSync(EXE)) {
  console.error(`Erreur : le simulateur Zephyr est introuvable : ${EXE}`);
  process.exit(1);
}
if (!fs.existsSync(PAGE)) {
  console.error(`Erreur : la page du dashboard est introuvable : ${PAGE}`);
  process.exit(1);
}

// « script » fournit un vrai terminal au shell Zephyr.
const ecu = spawn('script', ['-qfc', EXE, '/dev/null'], { stdio: ['pipe', 'pipe', 'inherit'] });
let restant = '';
const clients = new Set();
let arretEnCours = false;

ecu.stdout.setEncoding('utf8');
ecu.stdout.on('data', (bloc) => {
  restant += bloc;
  const lignes = restant.split('\n');
  restant = lignes.pop();
  for (const brute of lignes) traiterLigne(brute.replace(/\r/g, ''));
});

function traiterLigne(ligne) {
  if (ligne.includes('[DTC')) console.log(ligne);
  const trouve = ligne.match(/\{"t":[^}]*\}/);
  if (!trouve) return; // Les messages shell et les lignes incomplètes ne sont pas des mesures.
  try {
    const mesure = JSON.parse(trouve[0]);
    const message = `data: ${JSON.stringify(mesure)}\n\n`;
    for (const client of clients) client.write(message);
  } catch {
    // Une sortie mal formée est ignorée sans interrompre le flux.
  }
}

function repondre(res, code, texte = '') {
  res.writeHead(code, { 'Content-Type': 'text/plain; charset=utf-8' });
  res.end(texte);
}

const serveur = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (req.method === 'GET' && url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    fs.createReadStream(PAGE).pipe(res);
  } else if (req.method === 'GET' && url.pathname === '/api/ping') {
    repondre(res, 200, 'ok');
  } else if (req.method === 'GET' && url.pathname === '/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive'
    });
    res.write(': connexion établie\n\n');
    clients.add(res);
    req.on('close', () => clients.delete(res));
  } else if (req.method === 'POST' && url.pathname.startsWith('/fault/')) {
    const nom = url.pathname.slice('/fault/'.length);
    if (!['temp', 'oil', 'batt'].includes(nom)) return repondre(res, 404, 'Panne inconnue');
    if (!ecu.stdin.destroyed && ecu.exitCode === null) ecu.stdin.write(`ecu fault ${nom}\r`);
    res.writeHead(204);
    res.end();
  } else {
    repondre(res, 404, 'Introuvable');
  }
});

serveur.listen(PORT, () => console.log(`Dashboard ECU disponible sur http://localhost:${PORT}`));

function arreter() {
  if (arretEnCours) return;
  arretEnCours = true;
  serveur.close();
  // Ferme le processus enfant, puis nettoie un éventuel script/Zephyr resté en arrière-plan.
  if (ecu.exitCode === null && !ecu.killed) ecu.kill('SIGTERM');
  const nettoyage = spawn('pkill', ['-f', 'zephyr.exe'], { stdio: 'ignore' });
  nettoyage.on('error', () => {}); // pkill peut être absent ou ne trouver aucun processus.
  nettoyage.on('close', () => process.exit(0));
  setTimeout(() => process.exit(0), 1500).unref();
}

process.on('SIGINT', arreter);
process.on('SIGTERM', arreter);
ecu.on('error', (err) => {
  console.error(`Erreur au lancement du simulateur : ${err.message}`);
  arreter();
});
ecu.on('close', (code) => {
  console.log(`Le simulateur s'est arrêté (code ${code}).`);
  arreter();
});
