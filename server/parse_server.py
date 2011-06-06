#!/usr/bin/python
# coding: utf-8

from xml.etree import ElementTree as ET
import urllib2
import re
from time import mktime, strptime, sleep
import socket, json

tags_arr = {u'Дизайн': 0, u'Программирование': 1, u'Веб-строй': 2, u'Раскрутка': 3, u'Тексты и переводы': 4, u'Верстка': 5, u'Flash': 6, u'Логотипы': 7, u'Иллюстрации': 8, u'3D': 9, u'Аудио/Видео': 10, u'Иконки': 11, u'Разное': 12, u'Фото': 13, u'Консалтинг': 14, u'Маркетинг': 15, u'Администрирование': 16,}
#saits_arr = {u'flance.ru': 0, u'free-lance.ru': 1, u'weblancer.net': 2, u'freelancejob.ru': 3, u'freelance.ru': 4, u'free-lancers.net': 5, u'dalance.ru': 6, u'netlancer.ru': 7, u'vingrad.ru': 8, u'best-lance.ru': 9, u'free-lancing.ru': 10, u'freelance.tomsk.ru': 11, u'freelancehunt.com': 12, u'webfreelance.ru': 13, u'virtuzor.ru': 14, u'revolance.ru': 15, u'freelancerbay.com': 16, u'flance_ru.livejournal.com': 17, u'podrabotka.livejournal.com': 18, u'ru_freelance.livejournal.com': 19, u'ru_perevod4ik.livejournal.com': 20, u'ydalen_ru.livejournal.com': 21,}
money_rate = {"$": 30, "€": 40, "FM": 35, "руб": 1}

# Separator between field is symbol "&", end of the string -- ";"

PORT = 23142

url = "http://www.flance.ru/rss.xml"
get_id = re.compile(r"^.*project(\d+)$")
get_tags = re.compile(ur"<a[^>]*>([^<]+)</a>")
get_money1 = re.compile(ur"^ *<b class=\"black\"> *([0-9]+) *</b> *(\$|€|FM|руб) *</div>")
get_money2 = re.compile(ur"^ *<b class=\"black\"> *\(Бюджет: *([0-9]+) *(\$|€|FM|руб)\) *</b> *</div>")
zero_money = re.compile(ur"^ *0[^0-9]")


sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("", PORT))


update_interval = 15

all_projects = {}
download_stek = []
download_stek_size = 0
last_timestamp = 0
maked = []
maked_size = 10	# How many projects can be added simultaneously
for i in range(maked_size):
	maked.append(0)
maked_first = maked_size-1
my_id = 0
last_send_id = -1

def escape_str(string):
	return string.replace("&", "\\&").replace(";", "\\;")

while 1:
	try:
		result = urllib2.urlopen(url)
		handl = ET.parse(result)
		messages = handl.findall("channel/item")
		messages.reverse()
		for message in messages:
			guid = message.find('guid').text
			id = get_id.search(guid).group(1)
			
			try:
				now_timestamp = mktime(strptime(message.find('pubDate').text, "%a, %d %b %Y %H:%M:%S +0400")) - 4 * 3600
			except ValueError:
				log_fh = open("parse_server_log", "a+")
				log_fh.write("Couldn't parse time string -- '" + message.find('pubDate').text + "'\n")
				log_fh.close()
				now_timestamp = 0
			if now_timestamp >= last_timestamp:
				processed = 1
				for i in range(maked_size):
					if id == maked[i]:
						processed = 0
						break
				if processed == 0:
					continue
				maked_first = (maked_first+1)%maked_size
				maked[maked_first] = id
				all_projects[my_id] = {}
				all_projects[my_id]['title'] = message.find('title').text
				all_projects[my_id]['description'] = message.find('description').text
				all_projects[my_id]['pubDate'] = now_timestamp
				all_projects[my_id]['parsed'] = 0
				all_projects[my_id]['id'] = my_id
			
				download_stek.append([guid, my_id])
				download_stek_size += 1
				last_timestamp = now_timestamp
				my_id += 1
	except urllib2.URLError, e:
		# Internet problems
		pass
	except xml.etree.ElementTree.ParseError:
		# Damaged XML
		pass
	for i in range(download_stek_size):
		url_tmp = download_stek[0][0]
		id = download_stek[0][1]
		try:
			result = urllib2.urlopen(url_tmp).read().decode("utf-8")
			# Parse budget
			next = result.find(u'middleSide')
			result = result[next:]
			next = result.find(u'Бюджет:')
			if next > 0:
				result = result[next+7:]
				money_str = result[:100]
				#get_money1
				#next = result.find('Бюджет:')
				#if next > 0:
					#fh = open("log_to_budjet", "a+")
					#fh.write("Duble budget " + url_tmp + "\n")
					#fh.close()
					##result = result[next+7:]
					##next = result.find(')')
					##money = result[0:next].strip().replace('  ',' ').split(' ')
					##money_int = int(money[1])
					##money_type = money[2].replace(' ','')
				##else:
				money_str = money_str.replace(u"&euro;", u"€")
				money_str = money_str.replace(u"&#x0024;", u"$")
				if zero_money.match(money_str) is None:
					money = get_money1.match(money_str)
					if money is not None:
						money_int = int(money.group(1))
						money_type = money.group(2).encode("utf-8")
					else:
						money = get_money2.match(money_str)
						if money is not None:
							money_int = int(money.group(1))
							money_type = money.group(2).encode("utf-8")
						else:
							fh = open("parse_server_log", "a+")
							fh.write("Can not parse budget " + url_tmp + "\n")
							fh.close()
							money_int = 0
							# Default is rubbles
							money_type = 'руб'
				else:
					money_int = 0
					# Default is rubbles
					money_type = 'руб'
				
				
				try:
					money_int *= money_rate[money_type]
				except KeyError:
					log_fh = open("parse_server_log", "a+")
					log_fh.write("Unknown currency: '" + money_type + "' in " + url_tmp + "\n")
					log_fh.close()
					money_int = 0
				all_projects[id]['money'] = money_int
			else:
				all_projects[id]['money'] = 0
			
			# Parse categories
			next = result.find(u'class="tags"')
			result = result[next:]
			next = result.find(u':')
			result = result[next+1:]
			next = result.find(u'</div>')
			tags = result[0:next].strip()
			tags = get_tags.findall(tags)
			all_projects[id]['categ'] = []
			for tag in tags:
				try:
					all_projects[id]['categ'].append(tags_arr[tag])
				except KeyError:
					log_fh = open("parse_server_log", "a+")
					log_fh.write("Unknown category: '" + tag.encode('utf-8') + "' in " + url_tmp + "\n")
					log_fh.close()
			all_projects[id]['categ'].insert(0, len(all_projects[id]['categ']))
			
			# Parse link
			next = result.find(u'class="infopanel"')
			if next > 0:
				result = result[next:]
				#next = result.find('alt=\'')
				#result = result[next+5:]
				#next = result.find('\'')
				#sait = result[0:next].strip()
				#try:
					#all_projects[id]['sait'] = saits_arr[sait.decode('utf-8')]
				#except KeyError:
					#print "Ou NO!!!"# write in to log
				next = result.find(u'<a')
				result = result[next:]
				next = result.find(u'href')
				result = result[next:]
				next = result.find(u'"')
				result = result[next+1:]
				next = result.find(u'"')
				link = result[0:next].strip()
				all_projects[id]['link'] = link.encode('utf-8')
			else:
				all_projects[id]['link'] = url_tmp
			all_projects[id]['parsed'] = 1
			
			
			download_stek.pop(0)
			download_stek_size -= 1
		except urllib2.URLError, e:
			pass
	for id in range(last_send_id+1, my_id):
		if all_projects[id]['parsed'] == 1:
			del all_projects[id]['parsed']
			tmp = str(id) + '&' + escape_str(json.dumps(all_projects[id])) + '&' + str(all_projects[id]['money']) + '&' + json.dumps(all_projects[id]['categ']) + ';'
			try:
				json.loads(json.dumps(all_projects[id]))
			except:
				fh = open("json_problems", "a+")
				fh.write(tmp+"\n")
				fh.close()
			sock.send(tmp)
			#fh = open("bad_projects3", "a+")
			#fh.write(tmp+"\n\n\n")
			#fh.close()
			#print str(id)+" ("+str(all_projects[id]['money'])+") ("+str(all_projects[id]['categ'])+") '"+all_projects[id]["title"]+"'"
			#open("log", "a+").write((json.dumps(all_projects[id])+end_of_the_string))
			#fh.write((json.dumps(all_projects[id])+end_of_the_string)+"\n")
			#print id
			#if id == 19:
				#fh.close()
			del all_projects[id]
			last_send_id += 1
		else:
			break
	
	sleep(update_interval)
	
