# Dashboard ECU Zephyr

Cette page affiche les mesures du simulateur `native_sim` en temps réel. Le serveur Node.js crée un pseudo-terminal avec `script`, lit les lignes JSON émises par Zephyr, puis les diffuse au navigateur avec Server-Sent Events. Les boutons de panne envoient les commandes au shell Zephyr via ce terminal. Sans serveur, la page ouverte directement utilise son mode démonstration.

## Prérequis

- WSL Ubuntu avec Zephyr et le simulateur compilé à `~/zephyrproject/build/ecu_monitor/zephyr/zephyr.exe`.
- Node.js 18 ou plus récent, et la commande système `script` (généralement fournie par `util-linux`).
- Le navigateur et le serveur sur la même machine.

## Lancement

Dans WSL Ubuntu :

```sh
cd /mnt/c/ibtihelpersonnel/projetzephyr/dashboard
node server.js
```

Ouvrir ensuite <http://localhost:3000>. Pour utiliser npm, `npm start` lance le même serveur. Arrêter avec Ctrl+C.

## Dépannage

- **Simulateur introuvable** : compiler Zephyr puis vérifier le chemin indiqué dans le message d'erreur et `~/zephyrproject`.
- **`script` introuvable** : installer l'outil système avec `sudo apt install util-linux`.
- **Port 3000 occupé** : arrêter l'autre serveur qui l'utilise avant de relancer.
- **Pannes sans effet** : vérifier que le shell Zephyr est activé dans la configuration du simulateur.
