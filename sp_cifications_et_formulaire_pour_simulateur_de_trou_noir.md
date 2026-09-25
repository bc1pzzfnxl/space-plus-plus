# Spécifications Fondamentales & Formulaire Mathématique pour Simulateur de Trou Noir

Ce document rassemble les constantes, les métriques fondamentales de la relativité générale, les équations géodésiques, les calculs de transport radiatif (ray tracing), la thermodynamique quantique, ainsi que le cadre épistémologique nécessaire à l'implémentation d'un moteur de simulation relativiste.

## 1. Constantes Fondamentales et Unités Géométrisées

Pour simplifier le calcul numérique, on utilise fréquemment le système d'unités géométrisées où $G = c = \hbar = k_B = 1$.

| Constante | Symbole | Valeur SI | Valeur Géométrisée | 
 | ----- | ----- | ----- | ----- | 
| Vitesse de la lumière dans le vide | $c$ | $2{,}99792458 \times 10^8\ \text{m}\cdot\text{s}^{-1}$ | $1$ | 
| Constante de gravitation | $G$ | $6{,}67430 \times 10^{-11}\ \text{m}^3\cdot\text{kg}^{-1}\cdot\text{s}^{-2}$ | $1$ | 
| Constante de Planck réduite | $\hbar$ | $1{,}0545718 \times 10^{-34}\ \text{J}\cdot\text{s}$ | $1$ | 
| Constante de Boltzmann | $k_B$ | $1{,}380649 \times 10^{-23}\ \text{J}\cdot\text{K}^{-1}$ | $1$ | 
| Masse solaire | $M_\odot$ | $1{,}98847 \times 10^{30}\ \text{kg}$ | $1{,}4766\ \text{km}$ | 

## 2. Métriques Fondamentales de l'Espace-Temps

Dans une variété pseudo-riemannienne à 4 dimensions avec signature $(-, +, +, +)$, l'élément de ligne s'écrit :

$$
ds^2 = g_{\mu\nu} dx^\mu dx^\nu
$$

### 2.1 Trou noir statique sans charge : Métrique de Schwarzschild

Décrit un trou noir statique à symétrie sphérique de masse $M$ :

$$
ds^2 = -\left(1 - \frac{2GM}{c^2 r}\right) c^2 dt^2 + \left(1 - \frac{2GM}{c^2 r}\right)^{-1} dr^2 + r^2 (d\theta^2 + \sin^2\theta\, d\phi^2)
$$

* Rayon de Schwarzschild (Horizon des événements) :
  

  $$
  r_s = \frac{2GM}{c^2}
  $$

* Sphère de photons (orbite instable des photons) :
  

  $$
  r_{\text{ph}} = \frac{3GM}{c^2} = 1{,}5\, r_s
  $$

* Dernière orbite circulaire stable (ISCO) pour une particule massive :
  

  $$
  r_{\text{ISCO}} = \frac{6GM}{c^2} = 3\, r_s
  $$

### 2.2 Trou noir en rotation : Métrique de Kerr

Décrit un trou noir stationnaire à symétrie axiale, caractérisé par sa masse $M$ et son moment cinétique $J$.
Paramètre de spin sans dimension : $a_* = \frac{J c}{G M^2} \in [-1, 1]$.
Paramètre de Kerr : $a = \frac{J}{M c}$.

En coordonnées de Boyer-Lindquist $(t, r, \theta, \phi)$ :

$$
ds^2 = -\left(1 - \frac{2Mr}{\rho^2}\right) dt^2 - \frac{4Mar\sin^2\theta}{\rho^2} dt\,d\phi + \frac{\rho^2}{\Delta} dr^2 + \rho^2 d\theta^2 + \left(r^2 + a^2 + \frac{2Mra^2\sin^2\theta}{\rho^2}\right)\sin^2\theta\, d\phi^2
$$


*(avec* $G=c=1$*)*

Fonctions auxiliaires :

$$
\Delta(r) = r^2 - 2Mr + a^2
$$

$$
\rho^2(r, \theta) = r^2 + a^2 \cos^2\theta
$$

* **Horizon des événements (externe et interne) :** racines de $\Delta(r) = 0$
  

  $$
  r_\pm = M \pm \sqrt{M^2 - a^2}
  $$

* **Ergosphère (limite statique) :** surface où $g_{tt} = 0$
  

  $$
  r_E(\theta) = M + \sqrt{M^2 - a^2 \cos^2\theta}
  $$

  
  *(Dans l'ergorégion* $r_+ < r < r_E(\theta)$*, aucun observateur ne peut rester stationnaire par rapport à l'infini : effet d'entraînement des référentiels ou frame-dragging).*

* **ISCO pour Kerr :**
  

  $$
  r_{\text{ISCO}} = M \left(3 + Z_2 \mp \sqrt{(3 - Z_1)(3 + Z_1 + 2Z_2)}\right)
  $$

  
  avec signe $(-)$ pour orbite prograde et $(+)$ pour rétrograde :
  

  $$
  Z_1 = 1 + (1 - a_*^2)^{1/3} \left[(1 + a_*)^{1/3} + (1 - a_*)^{1/3}\right]
  $$

  $$
  Z_2 = \sqrt{3a_*^2 + Z_1^2}
  $$

## 3. Dynamique des Particules et Trajectoires (Ray Tracing)

### 3.1 Équation des Géodésiques

Les géodésiques d'une particule ou d'un photon de 4-vitesse $u^\mu = \frac{dx^\mu}{d\lambda}$ satisfont :

$$
\frac{d^2 x^\mu}{d\lambda^2} + \Gamma^\mu_{\alpha\beta} \frac{dx^\alpha}{d\lambda} \frac{dx^\beta}{d\lambda} = 0
$$


avec les symboles de Christoffel :

$$
\Gamma^\mu_{\alpha\beta} = \frac{1}{2} g^{\mu\sigma} \left(\partial_\alpha g_{\beta\sigma} + \partial_\beta g_{\alpha\sigma} - \partial_\sigma g_{\alpha\beta}\right)
$$

Pour les photons (géodésiques de genre nul) :

$$
g_{\mu\nu} \frac{dx^\mu}{d\lambda} \frac{dx^\nu}{d\lambda} = 0
$$

### 3.2 Formalisme de Hamilton-Jacobi (Intégration de Carter pour Kerr)

Dans la métrique de Kerr, quatre constantes du mouvement existent :

1. La masse au repos $\mu$ ($\mu = 0$ pour un photon).

2. L'énergie à l'infini $E = -p_t$.

3. Le moment angulaire axial $L_z = p_\phi$.

4. La **constante de Carter** $Q$.

Paramètres d'impact réduits :

$$
\xi = \frac{L_z}{E}, \quad \eta = \frac{Q}{E^2}
$$

Équations du premier ordre découplées (temps de Mino $d\tau = \frac{d\lambda}{\rho^2}$) :

$$
\frac{dr}{d\tau} = \pm \sqrt{\mathcal{R}(r)}
$$

$$
\frac{d\theta}{d\tau} = \pm \sqrt{\Theta(\theta)}
$$

$$
\frac{d\phi}{d\tau} = -\frac{a E - L_z/\sin^2\theta}{\rho^2} + \frac{a}{\Delta}\left[E(r^2+a^2) - a L_z\right]
$$

$$
\frac{dt}{d\tau} = -a(a E \sin^2\theta - L_z) + \frac{r^2 + a^2}{\Delta}\left[E(r^2+a^2) - a L_z\right]
$$

avec :

$$
\mathcal{R}(r) = \left[(r^2 + a^2) - a\xi\right]^2 - \Delta \left[\eta + (\xi - a)^2\right]
$$

$$
\Theta(\theta) = \eta + a^2 \cos^2\theta - \xi^2 \cot^2\theta
$$

## 4. Rendu Optique : Décalage Spectral et Réflectivité

### 4.1 Décalage vers le rouge (Facteur $g$)

Le rapport de fréquence entre l'émission ($e$) et la réception ($o$) est :

$$
g = \frac{\nu_o}{\nu_e} = \frac{(u^\mu p_\mu)_o}{(u^\nu p_\nu)_e}
$$

Pour un disque d'accrétion fin képlérien dans le plan équatorial :

* Vitesse angulaire orbitale :
  

  $$
  \Omega_K = \frac{d\phi}{dt} = \frac{1}{r^{3/2} + a}
  $$

* 4-vitesse du fluide émetteur $u_e^\mu = u_e^t (1, 0, 0, \Omega_K)$ avec :
  

  $$
  u_e^t = \frac{1}{\sqrt{-(g_{tt} + 2\Omega_K g_{t\phi} + \Omega_K^2 g_{\phi\phi})}}
  $$

### 4.2 Invariance de l'Intensité Spécifique (Théorème de Liouville relativiste)

Le long d'un rayon lumineux sans absorption intermédiaire :

$$
\frac{I_\nu}{\nu^3} = \text{constante}
$$


L'intensité observée s'obtient directement par :

$$
I_o(\nu_o) = g^3\, I_e\left(\frac{\nu_o}{g}\right)
$$


Pour le flux bolométrique intégré :

$$
I_{\text{bol, o}} = g^4\, I_{\text{bol, e}}
$$

## 5. Thermodynamique et Évaporation Quantique

### 5.1 Grandeurs Thermodynamiques

* **Température de Hawking :**
  

  $$
  T_H = \frac{\hbar c^3}{8\pi G M k_B} \approx 6{,}17 \times 10^{-8} \left(\frac{M_\odot}{M}\right)\ \text{K}
  $$

* **Entropie de Bekenstein-Hawking :**
  

  $$
  S_{BH} = \frac{k_B c^3 A}{4 G \hbar} = \frac{k_B A}{4 \ell_P^2}
  $$

  
  où $A$ est l'aire de l'horizon ($A = 4\pi r_s^2 = 16\pi \frac{G^2 M^2}{c^4}$ pour Schwarzschild) et $\ell_P = \sqrt{\frac{G\hbar}{c^3}}$ est la longueur de Planck.

### 5.2 Taux de Perte de Masse et Durée de Vie

Par émission de corps noir selon la loi de Stefan-Boltzmann :

$$
\frac{dM}{dt} = -\frac{\alpha \hbar c^4}{G^2 M^2}
$$


(où $\alpha$ dépend des degrés de liberté des particules émises ; $\alpha \approx 2{,}011 \times 10^{-4}$ pour les seuls photons).

* **Temps d'évaporation complet d'un trou noir de Schwarzschild :**
  

  $$
  t_{\text{evap}} = \frac{5120\pi G^2 M^3}{\hbar c^4} \approx 2{,}1 \times 10^{67} \left(\frac{M}{M_\odot}\right)^3\ \text{ans}
  $$

## 6. Architecture Algorithmique pour un Moteur de Rendu

Pour implémenter la simulation dans un moteur (Vulkan / WebGL / C++ CUDA) :

```
Camera (Observateur à l'infini ou à r_obs)
   │
   ├──> Pour chaque pixel (x, y) de l'écran :
   │      1. Calculer le vecteur impulsion initial p^μ du photon (Ray Casting arrière)
   │      2. Intégrer les équations géodésiques via Runge-Kutta 4 (ou Dormand-Prince)
   │      │
   │      ├──> Condition r < r_+ + ε :
   │      │      -> Le rayon tombe dans l'horizon (Pixel Noir = Ombre)
   │      │
   │      ├──> Intersection avec z = 0 (Plan équatorial / Disque d'accrétion) :
   │      │      -> Si r_ISCO <= r <= r_out :
   │      │            Calculer facteur g (Doppler + Redshift gravitationnel)
   │      │            Calculer profil de brillance I_e(r) (ex: Shakura-Sunyaev)
   │      │            Accumuler couleur et flux : I_pixel += g^4 * I_e
   │      │
   │      └──> Condition r > r_max (Échappement vers la voûte céleste) :
   │             -> Échantillonner la cubemap de l'espace profond
   │
   └──> Affichage / Tone-mapping HDR

```

## 7. Cadre Épistémologique et Métaphysique

L'intégration de la métaphysique dans une simulation relativiste soulève plusieurs frontières conceptuelles fondamentales :

1. **La censure cosmique de Penrose :** Les singularités physiques doivent rester dissimulées derrière un horizon des événements. La simulation ne peut calculer l'intérieur au-delà de la surface de Cauchy sans perdre le déterminisme.

2. **La relativité de la simultanéité :** Pour un observateur lointain, un objet met un temps coordonné infini ($t \to \infty$) pour franchir l'horizon, tandis que pour le voyageur, le franchissement a lieu en un temps propre fini $\tau$. Le moteur doit distinguer explicitement le temps coordonné de la scène du temps propre de l'observateur.

3. **Le principe holographique et perte d'information :** L'entropie d'un trou noir étant proportionnelle à sa surface et non à son volume, toute l'information tridimensionnelle du volume englobé est encodable sur la frontière bidimensionnelle de l'horizon.