int nyc_assembly_data_value = 42;
int *nyc_assembly_data_pointer = &nyc_assembly_data_value;
int nyc_assembly_data_first = 13;
int nyc_assembly_data_second = 17;
struct nyc_assembly_data_pair { int *first; int *second; };
struct nyc_assembly_data_pair nyc_assembly_data_pair = { .second = &nyc_assembly_data_second, .first = &nyc_assembly_data_first };
int main(void) { return *nyc_assembly_data_pointer != 42 || *nyc_assembly_data_pair.first != 13 || *nyc_assembly_data_pair.second != 17; }
