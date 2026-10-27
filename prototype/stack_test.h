/* Keep the post-recursion access: it prevents a tail-call loop from hiding
 * missing stack growth. Touch every page and retain each frame until return. */
static volatile DWORD stack_test_checksum;
__attribute__((noinline)) static void consume_stack(DWORD depth)
{
    volatile BYTE page[1024];
    for(unsigned i=0;i<sizeof(page);i++)page[i]=(BYTE)(depth+i);
    if(depth)consume_stack(depth-1);
    stack_test_checksum+=page[(depth+7)&1023];
}
static DWORD guest_stack_limit(void)
{ DWORD limit;__asm__ volatile("movl %%fs:8,%0":"=r"(limit));return limit; }
static BOOL check_stack_growth(void)
{
    DWORD before=guest_stack_limit();consume_stack(256);
    return stack_test_checksum&&guest_stack_limit()<before;
}
