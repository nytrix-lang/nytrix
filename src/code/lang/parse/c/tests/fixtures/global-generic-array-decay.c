/*
 * Fixture: apply generic selection to an array expression and its decay.
 */
struct nyc_generic_node {
  int value;
};

static struct nyc_generic_node *nyc_generic_nodes[2] = {0, 0};
static struct nyc_generic_node **nyc_selected_nodes =
    _Generic(nyc_generic_nodes,
    struct nyc_generic_node **: nyc_generic_nodes,
    default: 0);

int main(void) { return nyc_selected_nodes != nyc_generic_nodes; }
