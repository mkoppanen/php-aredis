aredis
------

This is a primarily a test extension and is not meant for production. I wanted to try out offloading
IO to a separate thread inside a PHP request. There are some asynchronous event libraries for PHP 
but the problem is that they turn the PHP main thread into event loop, which is not really suitable for a web request.

The design on the extension is quite simple: when you create a new 'aredis' object a background thread
is started. The background thread runs a libevent event loop and waits for commands from the parent thread.
The parent thread sends the commands using a lock-free queue (liblfds) and uses eventfd to signal the event loop
about new events.
When the background thread receives an event, it processes it and pushes the results into a queue going
back to the main thread.

I am not sure if this pattern is that useful, but I guess it could be generalised a bit further to provide
IO offloading in PHP. You could use it roughly in this fashion:

    <?php
    /* Create new object, also creates background thread */
    $a = new aredis();

    /* Send commands to fetch keys */
    $a->get ("my_key");
    $a->get ("another_key");
    $a->get ("more_keys");

    /*
      Do some other processing here
    */

    /*
      When needed process the get responses 
    */
    while (($ev = $a->process_events ()))
      var_dump ($ev);