.. _legacy_host:

Legacy Host Application
#######################

.. warning::

   The Host Application is a legacy server application. New installations should
   use the prebuilt :doc:`Looking Glass IDD <install_idd>`. These build
   instructions are retained for the complete matching B7 stack described in
   :ref:`legacy_host_policy`.

.. note::

   `Host` in this context refers to the `Looking Glass (LG) host server application`,
   to which the LG client connects to, it thus does not refer to the `host OS`!

   A common setup is to have a `host OS` running a `guest VM`. In such a setup
   the `client application` runs on the `host OS`; and the `host application`
   on the `guest VM`.

   The term `Host application` was chosen over `Guest application`, because LG
   can be run in a VM to VM configuration, in which case both Host Application and
   Client Application are run inside VM Guests.

The Windows Host Application is no longer the recommended server implementation.
It is missing many of the features available in the Looking Glass IDD, such as:

* No additional video capture overhead;
* Usage without a monitor or dummy plug connected;
* Automatically resizing the guest to fit the client application viewport;
* HDR support; or
* Usage in virtual machines without GPU acceleration.

B7 is the last recommended version of the legacy host application for users who
require its non-capture mouse input workflow. Use the complete matching B7 stack
for that workflow rather than mixing B7 and current components.

The documentation is included here for completeness.


.. toctree::
   :maxdepth: 1

   Installation <install_host>
   Configuration <host_usage>
   Building from source <build_host>
