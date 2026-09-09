;; Embeddable demo: a tiny Nytrix program a C host can run.
fn add(int a, int b) int {
   a + b
}

def scale = 10
print(add(2, 3) * scale)
