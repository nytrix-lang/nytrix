module consumer(value)
use "./provider.ny"

fn value() int { provider_value() + 1 }
