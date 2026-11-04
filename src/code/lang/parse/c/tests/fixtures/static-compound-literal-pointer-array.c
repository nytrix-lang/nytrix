typedef unsigned int __u32;
typedef __u32 u32;

enum hwmon_sensor_types {
  hwmon_chip,
  hwmon_temp,
  hwmon_in,
  hwmon_curr,
  hwmon_power,
  hwmon_energy,
  hwmon_energy64,
  hwmon_humidity,
  hwmon_fan,
  hwmon_pwm,
  hwmon_intrusion,
  hwmon_max,
};

enum hwmon_temp_attributes { hwmon_temp_input = 1 };
enum hwmon_in_attributes { hwmon_in_input = 2 };
enum hwmon_power_attributes {
  hwmon_power_max = 0,
  hwmon_power_rated_max = 2,
  hwmon_power_crit = 5,
};
enum hwmon_energy_attributes { hwmon_energy_input = 3 };
enum hwmon_curr_attributes { hwmon_curr_crit = 4 };
enum hwmon_fan_attributes { hwmon_fan_input = 6 };

struct hwmon_channel_info {
  enum hwmon_sensor_types type;
  const u32 *config;
};

#define CHANNEL_INFO(type_, ...)                                               \
  (&(const struct hwmon_channel_info){                                         \
      .type = hwmon_##type_, .config = (const u32[]){__VA_ARGS__, 0}})

static const struct hwmon_channel_info *const channels[] = {
    CHANNEL_INFO(temp, (1UL << hwmon_temp_input)),
    CHANNEL_INFO(in, (1UL << hwmon_in_input)),
    CHANNEL_INFO(power, (1UL << hwmon_power_max) |
                            (1UL << hwmon_power_rated_max) |
                            (1UL << hwmon_power_crit)),
    CHANNEL_INFO(energy, (1UL << hwmon_energy_input)),
    CHANNEL_INFO(curr, (1UL << hwmon_curr_crit)),
    CHANNEL_INFO(fan, (1UL << hwmon_fan_input)),
    0,
};

int main(void) {
  return channels[0]->type != hwmon_temp ||
         channels[0]->config[0] != (1UL << hwmon_temp_input) ||
         channels[0]->config[1] != 0 || channels[1]->type != hwmon_in ||
         channels[1]->config[0] != (1UL << hwmon_in_input) ||
         channels[1]->config[1] != 0 || channels[2]->type != hwmon_power ||
         channels[2]->config[0] != ((1UL << 0) | (1UL << 2) | (1UL << 5)) ||
         channels[2]->config[1] != 0 || channels[3]->type != hwmon_energy ||
         channels[3]->config[0] != (1UL << hwmon_energy_input) ||
         channels[4]->type != hwmon_curr ||
         channels[4]->config[0] != (1UL << hwmon_curr_crit) ||
         channels[5]->type != hwmon_fan ||
         channels[5]->config[0] != (1UL << hwmon_fan_input) || channels[6] != 0;
}
