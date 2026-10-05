#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/*
 * La aplicacion lee el KY-018 cada CONFIG_DEMO_SAMPLE_INTERVAL_MS y muestra el
 * resultado por serie.
 * Los botones solo cambian la restriccion de energia: no cambian el intervalo,
 * el trabajo del ADC ni activan radio, LED o comunicaciones inalambricas.
 */
#define ADC_NODE DT_PATH(zephyr_user)
#define READS_PER_CYCLE 8
#define BUTTON_COUNT 2
#define BUTTON_ACTIVE 0 /* B1 */
#define BUTTON_SWEEP 1  /* B2 */

/* Pulsaciones mas proximas que esto se tratan como rebotes de la misma. */
#define DEBOUNCE_MS 200

/*
 * Para Zephyr, STOP0/1/2 son el mismo estado (PM_STATE_SUSPEND_TO_IDLE) y solo
 * los distingue el substate-id que declara stm32wl.dtsi. Se leen del Devicetree
 * en vez de escribir 1/2/3 a mano.
 */
#define STOP0_SUBSTATE DT_PROP(DT_NODELABEL(stop0), substate_id)
#define STOP1_SUBSTATE DT_PROP(DT_NODELABEL(stop1), substate_id)
#define STOP2_SUBSTATE DT_PROP(DT_NODELABEL(stop2), substate_id)
#define STOP_COUNT 3

/* La aplicacion compara substates con < y >, asi que depende de que en
 * stm32wl.dtsi el substate-id crezca con la profundidad del STOP.
 */
BUILD_ASSERT(STOP0_SUBSTATE < STOP1_SUBSTATE && STOP1_SUBSTATE < STOP2_SUBSTATE,
	     "los substate-id de STOP deben crecer con la profundidad");

/* Valor de "substate mas profundo permitido" que significa ningun STOP: la
 * CPU se queda en PM_STATE_ACTIVE. Los substate-id reales empiezan en 1.
 */
#define NO_STOP 0

static const uint8_t stop_substates[STOP_COUNT] = {
	STOP0_SUBSTATE, STOP1_SUBSTATE, STOP2_SUBSTATE,
};

/* sw0 y sw1 ya estan definidos por la NUCLEO en su Devicetree. B3 (sw2) no se
 * configura: sin interrupcion, pulsarlo no hace nada.
 */
static const struct gpio_dt_spec buttons[BUTTON_COUNT] = {
	[BUTTON_ACTIVE] = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
	[BUTTON_SWEEP] = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
};

static const struct adc_dt_spec light_adc = ADC_DT_SPEC_GET(ADC_NODE);

/* Cada boton necesita su callback. */
static struct gpio_callback button_callbacks[BUTTON_COUNT];
static struct k_work limit_work;

/*
 * Cuantas veces ha entrado el sistema en STOP0, STOP1 y STOP2 desde la ultima
 * muestra. Es la unica forma de *ver* la decision de la politica: la salida
 * del ADC es identica con los tres limites.
 */
static atomic_t stop_entries[STOP_COUNT];

/*
 * Zephyr llama a esta funcion justo antes de pm_state_set(), desde el hilo
 * idle, con un spinlock tomado y las interrupciones bloqueadas
 * (subsys/pm/pm.c). Por eso solo incrementa un contador: un printk aqui
 * retrasaria la entrada en STOP y bloquearia las interrupciones mientras tanto.
 */
static void pm_state_entered(enum pm_state state, uint8_t substate_id)
{
	if (state != PM_STATE_SUSPEND_TO_IDLE) {
		return;
	}

	if (substate_id == STOP0_SUBSTATE) {
		atomic_inc(&stop_entries[0]);
	} else if (substate_id == STOP1_SUBSTATE) {
		atomic_inc(&stop_entries[1]);
	} else if (substate_id == STOP2_SUBSTATE) {
		atomic_inc(&stop_entries[2]);
	}
}

/* report_substate = true selecciona substate_entry, que recibe tambien el
 * substate-id; sin el, solo se sabria que fue "suspend-to-idle".
 */
static struct pm_notifier stop_counter = {
	.substate_entry = pm_state_entered,
	.report_substate = true,
};

/* La interrupcion escribe que boton se pulso y el work de Zephyr lo lee.
 * atomic_t permite compartir ese valor sin una lectura/escritura a medias.
 * current_max solo lo usa el work, asi que el siguiente paso del barrido se
 * calcula alli y no en la interrupcion.
 * Al arrancar no hay ningun lock: Zephyr puede llegar hasta STOP2.
 */
static atomic_t pressed_button;
static uint8_t current_max = STOP2_SUBSTATE;

static void print_limit(uint8_t max_substate)
{
	if (max_substate == NO_STOP) {
		printk("Permitido: active (sin STOP)\n");
	} else {
		printk("Permitido: hasta suspend-to-idle/%u (STOP%u)\n",
		       (unsigned int)max_substate,
		       (unsigned int)(max_substate - STOP0_SUBSTATE));
	}
}

/*
 * Ajusta los locks de Zephyr para que la politica no pase de max_substate.
 * Un substate esta bloqueado si es mas profundo que el limite. Los locks son
 * contadores, asi que solo se toca lo que cambia: se suelta (put) lo que
 * deja de estar bloqueado y se toma (get) lo que pasa a estarlo. Con NO_STOP
 * quedan bloqueados los tres y la politica no encuentra ningun estado.
 */
static void set_stop_limit(uint8_t max_substate)
{
	if (max_substate == current_max) {
		return;
	}

	for (int i = 0; i < STOP_COUNT; i++) {
		uint8_t substate = stop_substates[i];
		bool was_locked = substate > current_max;
		bool now_locked = substate > max_substate;

		if (was_locked && !now_locked) {
			pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, substate);
		} else if (!was_locked && now_locked) {
			pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, substate);
		}
	}

	current_max = max_substate;
	print_limit(max_substate);
}

/*
 * Siguiente paso del barrido de B2: STOP0 -> STOP1 -> STOP2 -> STOP0. Desde
 * active (tras B1) el barrido vuelve a empezar en STOP0.
 */
static uint8_t next_sweep_limit(uint8_t max_substate)
{
	for (int i = 0; i < STOP_COUNT - 1; i++) {
		if (stop_substates[i] == max_substate) {
			return stop_substates[i + 1];
		}
	}

	return stop_substates[0];
}

static void apply_button(struct k_work *work)
{
	ARG_UNUSED(work);

	/* El cambio real se hace aqui, no dentro de la interrupcion GPIO. */
	if (atomic_get(&pressed_button) == BUTTON_ACTIVE) {
		set_stop_limit(NO_STOP);
	} else {
		set_stop_limit(next_sweep_limit(current_max));
	}
}

/* Callback para la pulsacion de los botones */
static void button_pressed(const struct device *port,
			   struct gpio_callback *callback, uint32_t pins)
{
	static uint32_t last_press_ms;
	uint32_t now = k_uptime_get_32();

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	/* Un rebote del contacto genera varios flancos por pulsacion. Antes era
	 * inofensivo porque repetir un boton no cambiaba nada; con el barrido,
	 * cada rebote avanzaria un paso.
	 */
	if (now - last_press_ms < DEBOUNCE_MS) {
		return;
	}
	last_press_ms = now;

	/* La interrupcion solo guarda el boton pulsado y solicita el trabajo. */
	for (int i = 0; i < BUTTON_COUNT; i++) {
		if (callback == &button_callbacks[i]) {
			atomic_set(&pressed_button, i);
			k_work_submit(&limit_work);
			return;
		}
	}
}

static int setup_buttons(void)
{
	int err = 0;
	for (int i = 0; i < BUTTON_COUNT; i++) {
		/* GPIO_INPUT conserva el pull-up y la polaridad declarados por la placa. */
		if (!gpio_is_ready_dt(&buttons[i])) {
			printk("Error al iniciar los GPIO. Boton: %d\n", i);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);			
		if (err != 0)
		{
			printk("Error al configurar el canal el boton %d. Error: %d\n", i, err);
			return err;
		}

		gpio_init_callback(&button_callbacks[i], button_pressed,
				   BIT(buttons[i].pin));

		/* Una pulsacion genera la interrupcion que tambien despierta al MCU. */
		err = gpio_add_callback(buttons[i].port, &button_callbacks[i]);

		if (err != 0) {
			printk("Error al configurar el callback del boton %d. Error: %d\n", i, err);
			return err;
		}

		err = gpio_pin_interrupt_configure_dt(&buttons[i],
						    GPIO_INT_EDGE_TO_ACTIVE);
		if (err != 0) {
			printk("Error al configurar la interrupcion del boton %d. Error: %d\n", i, err);
			return err;
		}
	}

	return 0;
}

static int read_light(uint16_t *average)
{
	uint16_t sample;
	uint32_t sum = 0;

	/* Zephyr completa canales, resolucion y ganancia desde el overlay. */
	struct adc_sequence sequence = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	int err = adc_sequence_init_dt(&light_adc, &sequence);
	if (err != 0) {
		return err;
	}

	/* Con cualquier limite se realizan las mismas ocho conversiones. */
	for (int i = 0; i < READS_PER_CYCLE; i++) {
		err = adc_read_dt(&light_adc, &sequence);
		if (err != 0) {
			return err;
		}
		sum += sample;
	}

	*average = sum / READS_PER_CYCLE;
	return 0;
}

/* Función de init separada del main. */
static int init(void)
{
	int err = 0;

	if (!adc_is_ready_dt(&light_adc)) {
		printk("Error al iniciar el ADC\n");
		return -ENODEV;
	}
	err = adc_channel_setup_dt(&light_adc);
	if (err != 0) {
		printk("Error al configurar el canal el ADC. Error: %d\n", err);
		return err;
	}

	pm_notifier_register(&stop_counter);

	/* El firmware empieza sin locks, como Zephyr por defecto. */
	k_work_init(&limit_work, apply_button);
	print_limit(current_max);

	err = setup_buttons();
	if (err != 0) {
		printk("Error al iniciar los botones. Error: %d\n", err);
		return err;
	}

	printk("App iniciada\n");
	return 0;	
}

int main(void)
{
	/* La siguiente muestra tiene una fecha fija para no acumular retrasos. */
	int64_t next_sample = k_uptime_get();
	uint32_t sample_count = 0;
	int status = 0;

	status =  init();
	if (status != 0)
	{
		printk("Error al iniciar la configuracion inicial. Error: %d\n", status);
		return status;
	}

	printk("B1: active; B2: barrido STOP0 -> STOP1 -> STOP2; B3: sin uso\n");
	printk("Intervalo: %d ms; tramo de sueno: %d us; ADC: A0/PB1\n",
	       CONFIG_DEMO_SAMPLE_INTERVAL_MS, CONFIG_DEMO_SLEEP_SLICE_US);

	while (1) {
		uint16_t raw = 0;

		/* Esta parte del ciclo es exactamente igual con los tres limites. */
		int err = read_light(&raw);

		/* atomic_clear devuelve el valor anterior: entradas desde la ultima
		 * muestra, y el contador queda a cero para el siguiente periodo.
		 */
		atomic_val_t stop0 = atomic_clear(&stop_entries[0]);
		atomic_val_t stop1 = atomic_clear(&stop_entries[1]);
		atomic_val_t stop2 = atomic_clear(&stop_entries[2]);

		if (err == 0) {
			sample_count++;
			printk("Muestra %u: %u | STOP0 %ld STOP1 %ld STOP2 %ld\n",
			       (unsigned int)sample_count, (unsigned int)raw,
			       stop0, stop1, stop2);
		} else {
			printk("Error al leer el ADC: %d\n", err);
		}

		next_sample += CONFIG_DEMO_SAMPLE_INTERVAL_MS;

		/* Mientras main duerme no hay espera activa. El hilo idle puede aplicar
		 * la politica PM, y la fecha absoluta mantiene el periodo de muestreo.
		 */
		if (CONFIG_DEMO_SLEEP_SLICE_US == 0) {
			k_sleep(K_TIMEOUT_ABS_MS(next_sample));
		} else {
			/* Experimento de residencia: la politica solo ve el tiempo
			 * hasta el siguiente timeout, asi que dormir a tramos cortos
			 * le impide elegir los STOP mas profundos aunque el periodo
			 * total sea el mismo.
			 */
			while (k_uptime_get() < next_sample) {
				k_sleep(K_USEC(CONFIG_DEMO_SLEEP_SLICE_US));
			}
		}
	}

	return 0;
}
