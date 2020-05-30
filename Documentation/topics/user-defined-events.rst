..
      Copyright 2020, Red Hat, Inc.

      Licensed under the Apache License, Version 2.0 (the "License"); you may
      not use this file except in compliance with the License. You may obtain
      a copy of the License at

          http://www.apache.org/licenses/LICENSE-2.0

      Unless required by applicable law or agreed to in writing, software
      distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
      WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
      License for the specific language governing permissions and limitations
      under the License.

      Convention for heading levels in Open vSwitch documentation:

      =======  Heading 0 (reserved for the title in a document)
      -------  Heading 1
      ~~~~~~~  Heading 2
      +++++++  Heading 3
      '''''''  Heading 4

      Avoid deeper levels because they do not render well.

===================================
User defined events and notification
====================================

**Note:** This feature is considered experimental.


Events are objects or messages that are used to notify other software
components for a state change. In openvswitch, multiple resources that
are involved in processing packets, undergo various state changes.
Many of these resources are monitored internally by Openvswitch in
various statistical counters for an instance like coverage counters,
perf metric counters etc. When there is poor performance in network I/O
through openvswitch or even to check if any of the resources is not
healthy, sometimes the communication exchange between system operator
and developer is lengthy in terms of:

  - having to rely on CLI but the information sometimes is too much
    to connect hotspot dots.
  - Debugging tools like gdb is more developer friendly and can not
    be used when live traffic is performance oriented.
  - Perf like tracing tool can help finding top consumers on system
    resources but it is from a system point of view.

Hence, troubleshooting negotiations kick off between support and engineering
teams by means of various patches and collated logs and processes can
sometimes become tedious. Finally, we lose opportunity to quickly confirm
the root cause for an issue.

Event library in Openvswitch is to rescue system operator from this
data pollution and provide an abstract view on existing resources
useful for troubleshooting. Here are the advantages in using event
library enable CLI for troubleshooting.

  - Less intrusiveness in enabling event API in runtime across
    available resources.
  - Operator could engage monitoring application for receiving
    event notifications asynchronously.
  - It abstracts the way the processing resources could be
    diagnosed using readable definition file and it is the input
    to populate various events.
  - Developer can apply its API in other Openvswitch components to
    create useful events for their troubleshooting. For an example,
    timer API can be used to measure time taken by one or more
    problematic functions.

Event library is an independent library within Openvswitch which provides
an abstract view of occurrence of an event in the data and control path
to the system operator, in a very less intrusive and asynchronous way.
Event creation and handling is implemented as shown in below figure.

::

                            +--------------+
                            |  ovs-appctl  |
                            |   event/*    |
                            +------+-------+
                                   |
                               [2] |
        +--------------------------V---------------------------+
        |                                                      |
        |                                 +---------------+    |
        |                                 |   datapath    |    |
        |                         OVS     |   resources   |    |
        |                        daemon   +-------|-------+    |
        |                                         |  resource  |
        |                                     [3] |  updates   |
        |                     evaluate            |            |
        |                     definition  ********V********    |
        |                  +------------->*   event_FOO   *    |
        |                  |              *****************    |
        |    event_init()  |                                   |
        |         |        |                                   |
        |         |        |                                   |
        +---------|--------|-----------------------------------+
                  |        |
              [1] |        | [4]
                  |        |
             +----V--------|---+         +---------------------+
             |      event  *   |----^--->|   user monitor app  |
             |      thread     |    |    |   (ovs-testeventd)  |
             +-----------------+    |    +---------------------+
                                    |
                                [5] |
                           event notification
                               (JSON RPC)

Event thread
------------
Event thread is created during the bring up of ovs-vswitchd. Required
resources for the event API is initialized (as [1] in fig) and thread
will enter infinite loop to handle any event which will be registered
by the user. This thread takes care of handling the event definition
for an instance, in case of conditional event to evaluate whether the
event resource value has satisfied the condition defined about it.
If there is notification channel registered (JSONRPC socket) for that
event, then this thread will send corresponding information about the
handled event. If there is no notification channel requested, event is
assumed not handled and further handling will be stopped.

Event CLI
---------
Event CLI provisions creation and injection of events inside Openvswitch
through ovs-appctl utility  (as [2] in fig). Following commands are
available to the user.

    ovs-appctl event/define <json input file>
        CLI used to create events.

    ovs-appctl event/undefine <event name>
        CLI used to destroy event.

    ovs-appctl event/flush
        CLI used to destroy all events registered in one go.

    ovs-appctl event/list
        CLI used to list all registered events and their current status.

Event types
-----------
Depending the purpose of creating the events, they can be classified into
below supported types.

  - Conditional events
  - Message events

Conditional events
~~~~~~~~~~~~~~~~~
Conditional events are asynchronous events when there is some condition
associated with the resources under monitoring is satisfied. User defines
what the condition for the event is. Until the condition of state change
is evaluated to true, the event is muted.

An example definition could be as below.

::
    {
      “name”: “<name of the event>”
      “type”: “conditional”
      "definition”: {
        “resource”: “<name of the resource>”,
        “match": “per_min”,
        “op”: “gt”,
        “value”: 200
      }
    }

Message events
~~~~~~~~~~~~~
Message events are workflow events which are notified to the user
when it is processed in the workflow, where it is injected. As long as
the event is registered in the workflow, processing of that event is
handled by the event library.

An example definition could be as below.

::
    {
      “name”: “<name of the event>”
      “type”: “message”
      “definition”: {
        “resource”: “<name of the resource>”
      }
    }

Event resources
---------------
Any event functions on basis of some input from the resource that it
monitors. So, Event API supports below resources in the datapath for
events to associate with (as [3] in fig).

coverage counters
function timers

Coverage counters
~~~~~~~~~~~~~~~~
Coverage library provisions API to create a counter at some place in
packet flow where that portion of code could be examined for how many
times getting executed and at what rates (in the past 1 min and 1 hour).

Event API enables conditional events creation and notification from these
counters.

Example event definition:

::
   {
      “name”: "poll_create_node”,
      “type”: "conditional”,
      “definition”: {
        “resource”: “coverage_counter”,
        “match”: “exact”,
        “op”: “gt”,
        “value”: 10
      },
      “notify”: “/tmp/event.sock”
   }

Timer
~~~~~
Lots of functions involved in moving packets from one port to the other
and every function has its own instructions. It is useful to find hot
spots through timing functions that are important and critical in packet
processing eg datapath functions for rx/tx, flow lookup and upcall
functions etc.

Event API enables timing a target function using stopwatch library and
informs the time taken by it as a message events every time the function
is called.

Example event definition:

::
   {
      “name”: “netdev_send”,
      “type”: “message”,
      “definition”: {
        “resource”: “timer”,
        “samples”: 50,
        “unit”: “ns”
      },
      “notify”: “/tmp/event.sock”
   }

Event definitions
-----------------
Event API recognizes event definitions in JSON format data. Based on
the type of the event and resource that it is associated with, JSON
descriptions will vary.

+-------------+------------+--------------------------------------------------+
| **Type**    | **Key**    | **Description**                                  |
+-------------+------------+--------------------------------------------------+
| Common      | name       | Name of the event (in STRING).                   |
|             +------------+--------------------------------------------------+
|             | type       | One of the below types (in STRING).              |
|             |            | - conditional                                    |
|             |            | - message                                        |
|             +------------+--------------------------------------------------+
|             | notify     | Name of JSON RPC socket (in STRING).             |
|             +------------+--------------------------------------------------+
|             | definition | Detail on the type (as OBJECT)                   |
+-------------+------------+--------------------------------------------------+
| Conditional | resource   | One the resources to associate with (as STRING). |
| definition  |            | - coverage_counter                               |
|             +------------+--------------------------------------------------+
|             | match      | One of the types of resource value to evaluate   |
|             |            | (in STRING).                                     |
|             |            | - exact                                          |
|             |            | - rate_min                                       |
|             |            | - rate_hour                                      |
|             +------------+--------------------------------------------------+
|             | op         | One of the types of operation in evaluation      |
|             |            | (in STRING).                                     |
|             |            | - eq                                             |
|             |            | - ne                                             |
|             |            | - gt                                             |
|             |            | - ge                                             |
|             |            | - lt                                             |
|             |            | - le                                             |
|             +------------+--------------------------------------------------+
|             | value      | Value to be checked (in INTEGER)                 |
+-------------+------------+--------------------------------------------------+
| Message     | resource   | One the resources to associate with (as STRING). |
| definition  |            | - timer                                          |
|             +------------+--------------------------------------------------+
|             | samples    | Number of samples be processed before handling   |
|             |            | message event (in INTEGER).                      |
|             +------------+--------------------------------------------------|
|             | unit       | Unit of resource value (in STRING).              |
|             |            | All applicable strings.                          |
|             |            | - ms                                             |
|             |            | - us                                             |
+-------------+------------+--------------------------------------------------+

Step to enable event API:
-------------------------
::
  ovs-vsctl --no-wait set Open_vSwitch .
    other_config:user_defined_event_enable=true

