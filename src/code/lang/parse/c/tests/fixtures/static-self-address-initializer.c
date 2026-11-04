struct nyc_self_address_node;

static struct nyc_self_address_node *nyc_self_address =
    (void *)&nyc_self_address;

int main(void) { return (void *)&nyc_self_address != nyc_self_address; }
