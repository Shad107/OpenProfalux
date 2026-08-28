/*
 * pfx_enrol.h — apprentissage "sans télécommande" (enrôlement d'une identité virtuelle 0x067).
 *
 * EXPÉRIMENTAL. Contrairement au clonage (capture + rejeu d'une vraie télécommande), ce module
 * GÉNÈRE une identité virtuelle PFX (famille 0x067, une des 63 clés du pool) et émet la trame
 * d'apprentissage. L'utilisateur choisit son modèle : le modèle fixe le TE d'émission et la
 * chorégraphie de gestes.
 *
 * Non prouvé sur moteur réel (question self-learn vs normal-learn). Le clonage reste la voie sûre.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Catalogue de modèles. L'index = l'id exposé à l'UI. */
typedef struct {
    const char *name;      /* libellé affiché */
    uint16_t    te_us;     /* TE d'émission (Profalux 455, FranciaFlex 415) */
    const char *routine;   /* "R6" | "R7" | "R8" — chorégraphie d'apprentissage */
} pfx_model_t;

int                 pfx_enrol_model_count(void);
const pfx_model_t  *pfx_enrol_model(int id);   /* NULL si hors borne */
uint16_t            pfx_enrol_model_te(int model);   /* TE d'émission du modèle (défaut 455) */

/* Boutons PFX 0x067 — directions corrigées d'après test réel FranciaFlex M4G (2026-08-28).
 * Partagés avec shutters pour piloter un volet virtuel. */
#define PFX_BTN_UP     0x2
#define PFX_BTN_STOP   0x4
#define PFX_BTN_DOWN   0x8

/* Construit la trame 66 bits on-air ('0'/'1', out[66]=NUL) pour une identité 0x067.
 * Retourne 0 si serial valide (famille 0x067), -1 sinon. Utilisé aussi par shutters
 * pour générer les trames d'un volet virtuel (compteur roulant géré par l'appelant). */
int pfx_enrol_frame(uint32_t serial, uint8_t button, uint16_t counter, char out[68]);

/* Identité virtuelle courante. */
typedef struct {
    bool     active;
    int      model;
    uint8_t  idx;        /* slot 0..62 dans le pool de clés */
    uint32_t serial;     /* 28-bit (serial & 0x3FF == 0x067) */
    uint16_t counter;    /* compteur roulant courant */
} pfx_ident_t;

void pfx_enrol_init(void);              /* charge l'identité depuis NVS (idempotent) */
bool pfx_enrol_get(pfx_ident_t *out);   /* copie l'identité courante ; retourne active */

/* Génère une identité fraîche pour `model`. Retourne l'index de slot [0,62] ou -1. */
int  pfx_enrol_new_identity(int model);
/* Oublie l'identité courante (efface l'état). */
void pfx_enrol_forget(void);

/* Émet la trame d'apprentissage (bouton LEARN, compteur figé) en rafale. 0 si OK. */
int  pfx_enrol_emit_learn(void);
/* Émet une commande après enrôlement : "up" | "stop" | "down" (compteur roulant). 0 si OK. */
int  pfx_enrol_cmd(const char *cmd);
