use std.core

fn test_monomorphic_dict_reads() {
    def rows = [
        {"name": "Alice", "dept": "eng", "salary": 120000},
        {"name": "Bob",   "dept": "eng", "salary": 95000},
        {"name": "Carol", "dept": "mkt", "salary": 85000},
        {"name": "Dave",  "dept": "mkt", "salary": 75000},
        {"name": "Eve",   "dept": "eng", "salary": 110000}
    ]

    mut total_salary = 0
    mut i = 0
    while i < len(rows) {
        def row = rows.get(i)
        def sal = row.get("salary", 0)
        total_salary = total_salary + sal
        i = i + 1
    }
    assert(total_salary == 485000, "monomorphic dict get matches expected sum")

    ;; Guarded miss path: shape change at same call site
    def odd_row = {"salary": 50000, "extra": true}
    assert(odd_row.get("salary", 0) == 50000, "guarded miss path on shape change works")
}

fn test_monomorphic_sequence_reads() {
    def lists = [
        [10, 20, 30],
        [40, 50, 60],
        [70, 80, 90]
    ]

    mut first_sum = 0
    mut i = 0
    while i < len(lists) {
        def sub = lists.get(i)
        first_sum = first_sum + sub.get(0, 0)
        i = i + 1
    }
    assert(first_sum == 120, "monomorphic sequence get matches expected sum")
}

test_monomorphic_dict_reads()
test_monomorphic_sequence_reads()
print("✓ monomorphic devirtualization tests passed")
