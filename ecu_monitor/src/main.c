#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <string.h>

struct sensor_msg {
    int32_t temp_c;
    int32_t rpm;
    int32_t speed;
    int32_t oil_x10;   /* pression d'huile en dixiemes de bar (32 = 3,2 bar) */
    int32_t batt_x10;  /* tension batterie en dixiemes de volt (138 = 13,8 V) */
};

enum { DTC_TEMP, DTC_RPM, DTC_OIL, DTC_BATT, DTC_COUNT };

static const char *const dtc_code[DTC_COUNT] = {
    "P0217", "P0219", "P0520", "P0562"
};
static const char *const dtc_label[DTC_COUNT] = {
    "Surchauffe moteur", "Sur-regime moteur",
    "Pression huile basse", "Batterie faible"
};

/* Etat partage entre le thread diagnostic et le shell */
struct ecu_state {
    struct sensor_msg last;
    bool active[DTC_COUNT];
    uint32_t count[DTC_COUNT];
};

static struct ecu_state state;
K_MUTEX_DEFINE(state_lock);

/* Pannes forcees depuis le shell (nombre de mesures restantes) */
static volatile int32_t fault_temp, fault_oil, fault_batt;

K_MSGQ_DEFINE(sensor_q, sizeof(struct sensor_msg), 10, 4);

#define STACK_SIZE 1024
K_THREAD_STACK_DEFINE(sensor_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(diag_stack, STACK_SIZE);
static struct k_thread sensor_thread_data;
static struct k_thread diag_thread_data;

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* nombre aleatoire entre lo et hi inclus */
static int32_t rnd(int32_t lo, int32_t hi)
{
    return lo + (int32_t)(sys_rand32_get() % (uint32_t)(hi - lo + 1));
}

static void sensor_thread(void *a, void *b, void *c)
{
    int32_t temp = 80, rpm = 1000, speed = 0;
    int32_t oil = 30, batt = 139;
    int32_t target = 2500;
    int32_t tick = 0;

    while (1) {
        tick++;

        /* toutes les 3 s : nouveau regime vise (15 % de chance de pic) */
        if (tick % 6 == 0) {
            target = (rnd(0, 99) < 8) ? 6500 : rnd(1200, 4200);
        }

        /* le regime se rapproche de la cible, avec un peu de bruit */
        rpm = clamp(rpm + (target - rpm) / 2 + rnd(-100, 100), 800, 6500);
        speed = clamp(speed + (rpm / 40 - speed) / 4 + rnd(-3, 3), 0, 180);

        /* temperature : thermostat vers 90 C, ou 115 C si panne forcee */
        int32_t t_target = (fault_temp > 0) ? 115 : 90 + (rpm - 2500) / 900;
        temp = clamp(temp + (t_target - temp) / 5 + rnd(-1, 1), 60, 130);

        /* huile : monte avec le regime, 1,0 bar si panne forcee */
        int32_t o_target = (fault_oil > 0) ? 10 : 16 + rpm / 220;
        oil = clamp(oil + (o_target - oil) / 2 + rnd(-1, 1), 4, 50);

        /* batterie : 13,9 V, ou 11,2 V si panne forcee */
        int32_t b_target = (fault_batt > 0) ? 112 : 139;
        batt = clamp(batt + (b_target - batt) / 3 + rnd(-1, 1), 100, 150);

        if (fault_temp > 0) { fault_temp--; }
        if (fault_oil > 0)  { fault_oil--; }
        if (fault_batt > 0) { fault_batt--; }

        struct sensor_msg msg = {
            .temp_c = temp, .rpm = rpm, .speed = speed,
            .oil_x10 = oil, .batt_x10 = batt,
        };

        k_msgq_put(&sensor_q, &msg, K_NO_WAIT);
        k_msleep(500);
    }
}

static void diag_thread(void *a, void *b, void *c)
{
    struct sensor_msg msg;

    while (1) {
        k_msgq_get(&sensor_q, &msg, K_FOREVER);

        bool cond[DTC_COUNT] = {
            msg.temp_c > 105,
            msg.rpm > 6000,
            msg.oil_x10 < 15,
            msg.batt_x10 < 118,
        };
        int8_t event[DTC_COUNT];

        k_mutex_lock(&state_lock, K_FOREVER);
        state.last = msg;
        for (int i = 0; i < DTC_COUNT; i++) {
            event[i] = 0;
            if (cond[i] && !state.active[i]) {
                state.active[i] = true;
                state.count[i]++;
                event[i] = 1;      /* le defaut vient d'apparaitre */
            } else if (!cond[i] && state.active[i]) {
                state.active[i] = false;
                event[i] = -1;     /* le defaut vient de disparaitre */
            }
        }
        k_mutex_unlock(&state_lock);

        /* une alerte seulement quand l'etat change */
        for (int i = 0; i < DTC_COUNT; i++) {
            if (event[i] != 0) {
                printk("[DTC %s] %s : %s\n", dtc_code[i], dtc_label[i],
                       event[i] > 0 ? "APPARU" : "RESOLU");
            }
        }

        /* une ligne JSON par mesure, lue par le dashboard */
        printk("{\"t\":%u,\"temp\":%d,\"rpm\":%d,\"speed\":%d,"
               "\"oil\":%d.%d,\"batt\":%d.%d}\n",
               k_uptime_get_32(), msg.temp_c, msg.rpm, msg.speed,
               msg.oil_x10 / 10, msg.oil_x10 % 10,
               msg.batt_x10 / 10, msg.batt_x10 % 10);
    }
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    struct ecu_state copy;

    k_mutex_lock(&state_lock, K_FOREVER);
    copy = state;
    k_mutex_unlock(&state_lock);

    shell_print(sh, "Temperature : %d C", copy.last.temp_c);
    shell_print(sh, "Regime      : %d tr/min", copy.last.rpm);
    shell_print(sh, "Vitesse     : %d km/h", copy.last.speed);
    shell_print(sh, "Huile       : %d.%d bar", copy.last.oil_x10 / 10,
                copy.last.oil_x10 % 10);
    shell_print(sh, "Batterie    : %d.%d V", copy.last.batt_x10 / 10,
                copy.last.batt_x10 % 10);
    return 0;
}

static int cmd_dtc(const struct shell *sh, size_t argc, char **argv)
{
    struct ecu_state copy;

    k_mutex_lock(&state_lock, K_FOREVER);
    copy = state;
    k_mutex_unlock(&state_lock);

    for (int i = 0; i < DTC_COUNT; i++) {
        shell_print(sh, "%s %-22s : %u fois (%s)", dtc_code[i], dtc_label[i],
                    copy.count[i], copy.active[i] ? "ACTIF" : "ok");
    }
    return 0;
}

static int cmd_fault(const struct shell *sh, size_t argc, char **argv)
{
    if (strcmp(argv[1], "temp") == 0) {
        fault_temp = 16;
    } else if (strcmp(argv[1], "oil") == 0) {
        fault_oil = 16;
    } else if (strcmp(argv[1], "batt") == 0) {
        fault_batt = 16;
    } else {
        shell_error(sh, "Usage : ecu fault <temp|oil|batt>");
        return -EINVAL;
    }
    shell_print(sh, "Panne simulee pendant 8 secondes");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ecu_cmds,
    SHELL_CMD(status, NULL, "Affiche les dernieres mesures", cmd_status),
    SHELL_CMD(dtc, NULL, "Affiche les codes defaut", cmd_dtc),
    SHELL_CMD_ARG(fault, NULL, "Force une panne : temp, oil ou batt",
                  cmd_fault, 2, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ecu, &ecu_cmds, "Diagnostic ECU", NULL);

int main(void)
{
    k_thread_create(&sensor_thread_data, sensor_stack, STACK_SIZE,
                    sensor_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_create(&diag_thread_data, diag_stack, STACK_SIZE,
                    diag_thread, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
    return 0;
}
