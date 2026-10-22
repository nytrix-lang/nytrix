refine portable.Positive = int where(value) { value > 0 }
refine portable.Tagged = str where(value) { value == "ok" }
