/*
 * C parser fixture for taking an address from an array of function pointers.
 */
struct node {
  int value;
};

static struct node *nodes[2];

static struct node **get_nodes(void) { return nodes; }

static struct node *get_one(void) { return &get_nodes()[0][0]; }

static void set_two(struct node *value) { get_nodes()[1] = value; }

int main(void) {
  struct node first;
  struct node second;
  nodes[0] = &first;
  if (get_one() != &first)
    return 1;
  set_two(&second);
  return nodes[1] != &second;
}
