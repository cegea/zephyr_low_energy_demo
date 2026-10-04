#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/*
 * La aplicacion lee el KY-018 cada segundo y muestra el resultado por serie.
 * Los botones solo cambian la restriccion de energia: no cambian el intervalo,
 * el trabajo del ADC ni activan radio, LED o comunicaciones inalambricas.
 */
#define ADC_NODE DT_PATH(zephyr_user)
#define READS_PER_CYCLE 8
#define BUTTON_COUNT 3

/* MODE_NONE solo se usa durante el arranque, antes de aplicar el primer modo. */
enum energy_mode {
	MODE_NONE = -1,
	MODE_REFERENCE,
	MODE_RESPONSE,
	MODE_SAVING,
};

/* sw0, sw1 y sw2 ya estan definidos por la NUCLEO en su Devicetree. */
static const struct gpio_dt_spec buttons[BUTTON_COUNT] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
};

static const struct adc_dt_spec light_adc = ADC_DT_SPEC_GET(ADC_NODE);

/* Cada boton necesita su callback. La peticion de latencia se conserva para
 * poder retirarla al abandonar el modo de respuesta rapida.
 */
static struct gpio_callback button_callbacks[BUTTON_COUNT];
static struct pm_policy_latency_request latency_request;
static struct k_work mode_work;

/* La interrupcion escribe requested_mode y el work de Zephyr lo lee.
 * atomic_t permite compartir ese valor sin una lectura/escritura a medias.
 */
static atomic_t requested_mode = MODE_SAVING;
static enum energy_mode current_mode = MODE_NONE;

static void set_energy_mode(enum energy_mode mode)
{
	if (mode == current_mode) {
		return;
	}

	/* Primero se retira la restriccion anterior. Cada get/add debe tener su
	 * put/remove para no dejar una restriccion acumulada por accidente.
	 */
	if (current_mode == MODE_REFERENCE) {
		pm_policy_state_all_lock_put();
	} else if (current_mode == MODE_RESPONSE) {
		pm_policy_latency_request_remove(&latency_request);
	}

	if (mode == MODE_REFERENCE) {
		/* Impide que la politica elija cualquiera de los estados STOP. */
		pm_policy_state_all_lock_get();
		printk("Modo: referencia\n");
	} else if (mode == MODE_RESPONSE) {
		/* Solo permite estados cuya salida cumpla la latencia solicitada. */
		pm_policy_latency_request_add(&latency_request,
					      CONFIG_DEMO_RESPONSE_LATENCY_US);
		printk("Modo: respuesta rapida\n");
	} else {
		/* Ahorro no añade restricciones: decide la politica de Zephyr. */
		printk("Modo: ahorro\n");
	}

	current_mode = mode;
}

static void change_mode(struct k_work *work)
{
	ARG_UNUSED(work);
	/* El cambio real se hace aqui, no dentro de la interrupcion GPIO. */
	set_energy_mode((enum energy_mode)atomic_get(&requested_mode));
}

static void button_pressed(const struct device *port,
			   struct gpio_callback *callback, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	/* La interrupcion solo guarda el boton pulsado y solicita el trabajo. */
	for (int i = 0; i < BUTTON_COUNT; i++) {
		if (callback == &button_callbacks[i]) {
			atomic_set(&requested_mode, i);
			k_work_submit(&mode_work);
			return;
		}
	}
}

static int setup_buttons(void)
{
	for (int i = 0; i < BUTTON_COUNT; i++) {
		/* GPIO_INPUT conserva el pull-up y la polaridad declarados por la placa. */
		if (!gpio_is_ready_dt(&buttons[i]) ||
		    gpio_pin_configure_dt(&buttons[i], GPIO_INPUT) != 0) {
			return -1;
		}

		gpio_init_callback(&button_callbacks[i], button_pressed,
				   BIT(buttons[i].pin));

		/* Una pulsacion genera la interrupcion que tambien despierta al MCU. */
		if (gpio_add_callback(buttons[i].port, &button_callbacks[i]) != 0 ||
		    gpio_pin_interrupt_configure_dt(&buttons[i],
						    GPIO_INT_EDGE_TO_ACTIVE) != 0) {
			return -1;
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

	/* Todos los modos realizan las mismas ocho conversiones. */
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

/*
	Función de init separada del main para capturar errores y hacer las
	operaciones atómicas.
*/
int init(void)
{
	int status = 0;

	if (!adc_is_ready_dt(&light_adc)) {
		printk("Error al iniciar el ADC\n");
		return ENODEV;
	}
	status = adc_channel_setup_dt(&light_adc);
	if (0 != status) {
		printk("Error al configurar el canal el ADC. Error: %d\n", status);
		return status;
	}

	/* El firmware siempre empieza sin restricciones, en modo ahorro. */
	k_work_init(&mode_work, change_mode);
	set_energy_mode(MODE_SAVING);

	status = setup_buttons();
	if (setup_buttons() != 0) {
		printk("Error al iniciar los botones\n. Error: %d", status);
		return status;
	}

	return status;	
}

int main(void)
{
	/* La siguiente muestra tiene una fecha fija para no acumular retrasos. */
	int64_t next_sample = k_uptime_get();
	uint32_t sample_count = 0;
	int status = 0;

	status =  init();
	if (0 != status)
	{
		printk("Error al iniciar la configuracion inicial. Error: %d\n", status);
	}

	printk("B1: referencia; B2: respuesta rapida; B3: ahorro\n");
	printk("Intervalo: %d ms; ADC: A0/PB1\n",
	       CONFIG_DEMO_SAMPLE_INTERVAL_MS);

	while (1) {
		uint16_t raw;

		/* Esta parte del ciclo es exactamente igual en los tres modos. */
		int err = read_light(&raw);

		if (err == 0) {
			sample_count++;
			printk("Muestra %u: %u\n", (unsigned int)sample_count,
			       (unsigned int)raw);
		} else {
			printk("Error al leer el ADC\n");
		}

		next_sample += CONFIG_DEMO_SAMPLE_INTERVAL_MS;

		/* Mientras main duerme no hay espera activa. El hilo idle puede aplicar
		 * la politica PM, y la fecha absoluta mantiene el periodo de muestreo.
		 */
		k_sleep(K_TIMEOUT_ABS_MS(next_sample));
	}

	return 0;
}
