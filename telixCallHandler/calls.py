#! /usr/bin/python3

import os
import time
import requests
import re
from concurrent.futures import ThreadPoolExecutor
from settings import login, connection

from asterisk.ami import AMIClient
from asterisk.ami import EventListener

executor = ThreadPoolExecutor(max_workers=1)

def send_bg(string):
    try:
        response = requests.post('http://localhost/cgi-bin/aid/call', string)
        print("%s: %s" % (response.status_code,response.text.rstrip()))
    except Exception as e:
        print(f"Unexpected error: {e}")


def send(string):
    print(string)
    executor.submit(send_bg, string)

def on_incomming_call(source, event):
    send('{"event": "Incoming Call", "remote": "%s", "callid": "%s", "dialed": "%s"}' % (event['CallerIDNum'], event['Uniqueid'], event['Exten']))

def on_hangup(source, event):
    send('{"event": "Hangup", "callid": "%s", "remote": "%s"}' % (event['Uniqueid'], event['CallerIDNum']))

def on_accept(source, event):
    if event['ConnectedLineName']=="<unknown>":
        send('{"event": "Accepted Call", "callid": "%s", "remote": "%s", "dialed": "%s"}' % (event['Uniqueid'], event['CallerIDNum'], event['Exten']))
    else:
        send('{"event": "Accepted Call", "callid": "%s", "remote": "%s", "dialed": "%s", "user": "%s"}' % (event['Uniqueid'], event['CallerIDNum'], event['Exten'], event['ConnectedLineName'].split(' ', 1)[0].lower()))

def on_outgoing_call(source, event):
    send('{"event": "Outgoing Call", "callid": "%s", "remote": "%s", "user": "%s"}' % (event['Uniqueid'], event['Exten'], event['CallerIDName'].split(' ', 1)[0].lower()))

def on_transfer(source, event):
    send('{"event": "Transfer Call", "callid": "%s", "newuser": "%s"}' % (event['TransfereeUniqueid'], event['TransferTargetCallerIDName'].split(' ', 1)[0].lower()))

def event_notification(source, event):
    print('%s %s' % (event.name, str(event)))

#    if event.ChannelState

client = AMIClient(**connection)
future = client.login(**login)
if future.response.is_error():
    raise Exception(str(future.response))

client.add_event_listener(EventListener(on_event=on_incomming_call, white_list='Newstate', ChannelStateDesc='Ring', Context='from-trunk', Channel=re.compile('^SIP/Sipgate_-2615510.*')))
client.add_event_listener(EventListener(on_event=on_outgoing_call, white_list='Newstate', ChannelStateDesc='Ring', Context='from-internal', Exten=re.compile('.{5,}')))
client.add_event_listener(EventListener(on_event=on_hangup, white_list='Hangup', Context= 'from-trunk'))
client.add_event_listener(EventListener(on_event=on_accept, white_list='Newstate', Context= 'from-trunk', ChannelStateDesc='Up'))
client.add_event_listener(EventListener(on_event=on_transfer, white_list='AttendedTransfer'))

#client.add_event_listener(EventListener(on_event=event_notification, white_list=['Newstate','Hangup'])) #, ChannelStateDesc='Ringing'))
#client.add_event_listener(EventListener(on_event=event_notification, black_list=['VarSet'])) #, ChannelStateDesc='Ringing'))

try:
    while True:
        time.sleep(10)
except (KeyboardInterrupt, SystemExit):
    client.logoff()


