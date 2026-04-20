int main() {
    char *argv[] = {"/bin/sh", NULL};

    ContainerArgs args;
    strcpy(args.rootfs, "../rootfs-alpha");
    strcpy(args.name, "test");
    args.cmd_argc = 1;
    strcpy(args.cmd[0], "/bin/sh");

    args.pipe_wr = STDOUT_FILENO;

    clone(container_main, clone_stack + STACK_SIZE,
          CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
          &args);

    wait(NULL);
}
