# cores
Cores is a Windows app to show CPU core states: busy, idle, or thermally throttled.

Cores.exe creates a window without a title bar or menu. You can move the window by clicking and dragging the window.

The window position is persisted in the registry when cores exits. When cores is restarted it is placed at its last position.

Exit cores by giving it focus and pressing ESC.

Cores updates its state five times a second. Core states can change in nanoseconds, so what you see is really just a snapshot.

Each physical core in your CPU is represented by a square. Each square is filled with a color:

    * Green: The core is at peak frequency and ran some code during the update time period.
    * Blue: The core is idle and at a low-power frequency.
    * Red: The core is thermally throttled to a lower frequency but still running.

Performance core squares are fully saturated. Progressively more slow/efficienct core squares are filled with less-saturated colors. 

Generally, cores are sorted from most performant to most efficient.

Here are some example cores app states for an Intel(R) Core(TM) i9-14900KF CPU (8 performance and 16 efficiency cores):

  ![Alt Text](images/1.png)
    
    Three performance cores are idle and five are at full speed running code (though perhaps not at 100% usage).
    All efficiency cores are idle.

----

   ![Alt Text](images/2.png)

    All cores are at full throttle and likely running code.

----

   ![Alt Text](images/4.png)

    A mix of performance and efficiency cores are busy or idle.

----

   ![Alt Text](images/6.png)

    All but one performance cores are active. All other cores are idle.

----

   ![Alt Text](images/8.png)

    All cores are busy, and one performance core is thermally throttled.
    Windows keeps threads pinned to throttled cores to retain cache entries.

----

   ![Alt Text](images/11.png)

    Performance cores are at idle, but background tasks assigned to efficiency cores are busy.
    
