;; Keywords: native incremental cache dependency fixture
;; The consumer module depends on provider; edit provider to verify invalidation.
use "./consumer.ny"

print(value())
