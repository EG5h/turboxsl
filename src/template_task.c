#include "template_task.h"

#include "xsl_elements.h"
#include "template_task_graph.h"

struct template_task_ {
    template_context *context;
    void (*function)(template_context *);
    shared_variable *workers;
};

void template_task_function(void *data)
{
    template_task *task = (template_task *)data;
    template_task_graph_set_current(task->context->context->task_graph, task->context);
    task->function(task->context);

    if (task->workers != NULL)
    {
        shared_variable_decrease(task->workers);
    }
}

void template_task_run(XMLNODE *instruction, template_context *context, void (*function)(template_context *))
{
    if (context->context->gctx->pool == NULL)
    {
        function(context);
        return;
    }

    if (context->task_mode == SINGLE)
    {
        debug("single task mode");
        function(context);
        return;
    }

    if (context->task_mode == DENY)
    {
        XMLSTRING fork = xml_get_attr(instruction, xsl_a_fork);
        if (!xmls_equals(fork, xsl_s_yes))
        {
            debug("deny task mode");
            template_task_graph_add_serial(context->context->task_graph, instruction, context);
            function(context);
            return;
        }
        debug("switch to default task mode");
        context->task_mode = DEFAULT;
    }
    else
    {
        XMLSTRING fork = xml_get_attr(instruction, xsl_a_fork);
        if (fork != NULL)
        {
            if (xmls_equals(fork, xsl_s_no))
            {
                debug("no fork");
                template_task_graph_add_serial(context->context->task_graph, instruction, context);
                function(context);
                return;
            }

            if (xmls_equals(fork, xsl_s_deny))
            {
                debug("switch to deny task mode");
                context->task_mode = DENY;
                template_task_graph_add_serial(context->context->task_graph, instruction, context);
                function(context);
                return;
            }
        }
        else
        {
            if (dict_find(context->context->parallel_instructions, instruction->name) == NULL)
            {
                debug("instruction not found in default list");
                template_task_graph_add_serial(context->context->task_graph, instruction, context);
                function(context);
                return;
            }
        }
    }

    debug("running in new task");
    template_task *task = memory_allocator_new(sizeof(template_task));
    task->context = context;
    task->function = function;
    // continue template task
    if (context->workers != NULL)
    {
        task->workers = context->workers;
        shared_variable_increase(task->workers);
    }

    template_task_graph_add_parallel(context->context->task_graph, instruction, context);
    threadpool_start(context->context->gctx->pool, template_task_function, task);
}

void template_task_run_and_wait(template_context *context, void (*function)(template_context *))
{
    if (context->context->gctx->pool == NULL)
    {
        function(context);
        return;
    }

    if (!thread_pool_try_wait(context->context->gctx->pool))
    {
        function(context);
        return;
    }

    // create new variable for path
    context->workers = shared_variable_create();

    template_task *task = memory_allocator_new(sizeof(template_task));
    task->context = context;
    task->function = function;
    task->workers = context->workers;

    template_task_graph_add_parallel(context->context->task_graph, NULL, context);
    threadpool_start(context->context->gctx->pool, template_task_function, task);

    shared_variable_wait(task->workers);
    shared_variable_release(task->workers);

    thread_pool_finish_wait(context->context->gctx->pool);
}
